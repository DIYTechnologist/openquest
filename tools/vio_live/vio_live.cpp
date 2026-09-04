// vio_live.cpp — close the loop: our cameras -> our VIO -> Meta's compositor, live on device.
//
// Step 4 (notes/18). Reads the binary record stream produced by `cam_kernel ... -`, runs OpenVINS
// on it, and injects each resulting pose through TrackingDataInjection (notes/23). No Meta
// userspace library is involved in the sensing or estimation path; the only Meta component is the
// compositor we are deliberately driving.
//
// Two processes rather than one, joined by a pipe: the capture side is C against the msm camera
// ABI, this side is C++ against OpenVINS. ~18.5 MB/s through the pipe is cheap next to the 31.75 ms
// the estimator costs per frame.
//
//   cam_kernel 4 <secs> 3000 160 02 -  |  vio_live <config.yaml> <imu_rect.txt> [--no-inject]
//
// ── Clocks ───────────────────────────────────────────────────────────────────────────────────
// The camera and IMU device clocks do NOT share an epoch: measured ~28112 s apart. The offline
// builder fits them with a least-squares pass over the whole capture, which a live consumer cannot
// do. Camera V4L2 stamps are already CLOCK_MONOTONIC, so only the IMU needs mapping, and every
// record carries the host monotonic time at which it was observed.
//
// The estimator uses min-offset tracking rather than a running least-squares fit: arrival lag is
// always POSITIVE (a sample can be observed late, never early), so min(host - dev) over a window
// converges on the true offset and is immune to the burstiness of SPI chunk reads, which is exactly
// what corrupts a naive average. The window decays slowly so genuine clock drift is still followed.

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <deque>
#include <memory>
#include <string>
#include <vector>

#include <dlfcn.h>
#include <signal.h>
#include <unistd.h>

#include <opencv2/opencv.hpp>

#include "core/VioManager.h"
#include "core/VioManagerOptions.h"
#include "state/State.h"
#include "utils/print.h"
#include "utils/sensor_data.h"

#include <android/binder_ibinder.h>
#include <android/binder_parcel.h>
#include <android/binder_status.h>

using namespace ov_msckf;

// ── injection (same interface as tools/pose_inject) ──────────────────────────────────────────
typedef AIBinder *(*get_service_fn)(const char *);
static get_service_fn AServiceManager_getService;
static AIBinder *g_svc = nullptr;

static void *cls_create(void *a) { return a; }
static void cls_destroy(void *) {}
static binder_status_t cls_transact(AIBinder *, transaction_code_t, const AParcel *, AParcel *) {
  return STATUS_UNKNOWN_TRANSACTION;
}

static bool inject_init() {
  void *h = dlopen("libbinder_ndk.so", RTLD_NOW);
  if (!h) { fprintf(stderr, "[-] dlopen libbinder_ndk: %s\n", dlerror()); return false; }
  AServiceManager_getService = (get_service_fn)dlsym(h, "AServiceManager_getService");
  if (!AServiceManager_getService) { fprintf(stderr, "[-] dlsym failed\n"); return false; }
  g_svc = AServiceManager_getService("TrackingDataInjection");
  if (!g_svc) { fprintf(stderr, "[-] TrackingDataInjection not found\n"); return false; }
  AIBinder_Class *c = AIBinder_Class_define("oculus.internal.virtual_input.ITrackingDataInjectionService",
                                            cls_create, cls_destroy, cls_transact);
  if (!AIBinder_associateClass(g_svc, c)) { fprintf(stderr, "[-] associateClass failed\n"); return false; }
  return true;
}

static bool inject_field(int field, const float *v, int n) {
  AParcel *in = nullptr, *out = nullptr;
  if (AIBinder_prepareTransaction(g_svc, &in) != STATUS_OK) return false;
  if (AParcel_writeInt32(in, field) != STATUS_OK) return false;
  if (AParcel_writeFloatArray(in, v, n) != STATUS_OK) return false;
  if (AIBinder_transact(g_svc, 2, &in, &out, 0) != STATUS_OK) return false;
  int32_t exc = 0, ok = 0;
  AParcel_readInt32(out, &exc); AParcel_readInt32(out, &ok);
  AParcel_delete(out);
  return exc == 0 && ok != 0;
}

// ── stream records (must match cam_kernel.c) ─────────────────────────────────────────────────
#pragma pack(push, 1)
struct rec { uint8_t type, cam; uint16_t w, h; uint64_t dev_ns, host_ns; uint32_t bytes; };
#pragma pack(pop)

static bool read_full(void *p, size_t n) {
  size_t got = 0;
  while (got < n) {
    ssize_t r = read(STDIN_FILENO, (char *)p + got, n - got);
    if (r <= 0) return false;
    got += (size_t)r;
  }
  return true;
}

// Min-offset clock tracker; see header comment for why min rather than mean.
class ClockMap {
  std::deque<std::pair<double, int64_t>> w_;   // (host_seconds, offset_ns)
  double win_;
public:
  explicit ClockMap(double window_s = 5.0) : win_(window_s) {}
  int64_t update(uint64_t dev_ns, uint64_t host_ns) {
    double t = host_ns * 1e-9;
    w_.emplace_back(t, (int64_t)host_ns - (int64_t)dev_ns);
    while (!w_.empty() && t - w_.front().first > win_) w_.pop_front();
    int64_t best = w_.front().second;
    for (auto &e : w_) if (e.second < best) best = e.second;
    return best;
  }
};

static volatile sig_atomic_t g_stop = 0;
static void on_sig(int) { g_stop = 1; }

static void load_rect(const char *path, double Rg[9], double og[3], double Ra[9], double oa[3]) {
  FILE *f = fopen(path, "r");
  if (!f) { perror(path); exit(1); }
  char line[1024];
  auto row = [&](double *dst, int n) {
    while (fgets(line, sizeof line, f)) {
      if (line[0] == '#') continue;
      char *p = line;
      for (int i = 0; i < n; i++) dst[i] = strtod(p, &p);
      return;
    }
  };
  row(Rg, 9); row(og, 3); row(Ra, 9); row(oa, 3);
  fclose(f);
}

int main(int argc, char **argv) {
  if (argc < 3) {
    fprintf(stderr, "usage: %s <config.yaml> <imu_rect.txt> [--no-inject]\n", argv[0]);
    return 2;
  }
  bool do_inject = true;
  for (int i = 3; i < argc; i++) if (!strcmp(argv[i], "--no-inject")) do_inject = false;
  signal(SIGINT, on_sig); signal(SIGTERM, on_sig);

  double Rg[9], og[3], Ra[9], oa[3];
  load_rect(argv[2], Rg, og, Ra, oa);

  auto parser = std::make_shared<ov_core::YamlParser>(argv[1]);
  VioManagerOptions params;
  params.print_and_load(parser);
  params.num_opencv_threads = 4;
  { std::string v = "SILENT"; ov_core::Printer::setPrintLevel(v); }
  auto sys = std::make_shared<VioManager>(params);

  if (do_inject && !inject_init()) return 1;
  fprintf(stderr, "[+] vio_live ready (inject=%d)\n", (int)do_inject);

  const double G = 9.80665, DEG = M_PI / 180.0;
  ClockMap imu_clock;
  int64_t imu_off = 0;
  bool have_off = false;

  // Camera timing. V4L2 stamps are CLOCK_MONOTONIC; the IMU is on the nRF clock; the two sit
  // ~816 ms apart (the offline builder's "cam shift"). The MCU's 0xe0 exposure stamps are on the
  // nRF clock, so they are the bridge.
  //
  // Rather than associate frame<->exposure one-to-one (which a single dropped frame would desync
  // permanently), estimate the CONSTANT offset between the two clocks and then use the V4L2 stamps,
  // whose relative timing is reliable. Median over a window, so an occasional bad pairing cannot
  // move it.
  std::deque<uint64_t> expo;
  std::deque<double> off_samples;
  double cam_off = 0; bool have_camoff = false;
  long n_expo = 0;

  // Stereo pairing: hold the most recent frame from each camera and pair on arrival.
  cv::Mat pend[4];
  double pend_t[4] = {0, 0, 0, 0};
  bool pend_ok[4] = {false, false, false, false};

  long n_imu = 0, n_frame = 0, n_pair = 0, n_inj = 0, n_injfail = 0;
  // How far AHEAD of each frame the IMU has advanced when that frame is fed. The offline runner
  // guarantees IMU_LEAD_S = 0.10 s of look-ahead; a live consumer can only feed what has arrived,
  // and if this is negative the estimator is being handed images it cannot yet propagate to.
  double t_last_imu = 0;
  std::vector<double> lead_ms;
  // IMU arriving out of device-time order would be silently discarded by the estimator: chunk
  // arrival is bursty, so a late chunk can carry samples older than an already-processed frame.
  double t_prev_imu = 0; long n_imu_ooo = 0, n_imu_stale = 0;
  double t_first_pose = -1, t_last_pose = -1;
  std::vector<double> proc_ms;

  struct rec r;
  std::vector<unsigned char> buf;
  while (!g_stop && read_full(&r, sizeof r)) {
    buf.resize(r.bytes);
    if (r.bytes && !read_full(buf.data(), r.bytes)) break;

    if (r.type == 'I') {
      if (r.bytes != 24) continue;
      const float *v = (const float *)buf.data();
      // Everything runs on the nRF clock. The IMU is already on it, and camera frames are mapped
      // onto it below via the 0xe0 exposure stamps -- so no mapping is needed here at all.
      // (Mapping the IMU to CLOCK_MONOTONIC while cameras stayed on nRF put the two streams 4318 s
      // apart and the estimator simply never initialised: two disjoint timelines, no error.)
      imu_off = imu_clock.update(r.dev_ns, r.host_ns);   // tracked for diagnostics only
      have_off = true;
      double t = (double)r.dev_ns * 1e-9;
      // raw gyro is deg/s, raw accel is g; rectify in raw units, then convert (same order as
      // build_euroc_direct.py, which is what the converged config was tuned against)
      double gr[3] = {v[0] - og[0], v[1] - og[1], v[2] - og[2]};
      double ar[3] = {v[3] - oa[0], v[4] - oa[1], v[5] - oa[2]};
      ov_core::ImuData m;
      m.timestamp = t;
      for (int i = 0; i < 3; i++) {
        m.wm(i) = (Rg[i*3+0]*gr[0] + Rg[i*3+1]*gr[1] + Rg[i*3+2]*gr[2]) * DEG;
        m.am(i) = (Ra[i*3+0]*ar[0] + Ra[i*3+1]*ar[1] + Ra[i*3+2]*ar[2]) * G;
      }
      if (t_prev_imu > 0 && t < t_prev_imu) n_imu_ooo++;
      if (n_pair > 0 && t < pend_t[0]) n_imu_stale++;
      t_prev_imu = t;
      sys->feed_measurement_imu(m);
      if (t > t_last_imu) t_last_imu = t;
      n_imu++;
      continue;
    }
    if (r.type == 'E') {
      expo.push_back(r.dev_ns);
      if (expo.size() > 240) expo.pop_front();
      n_expo++;
      continue;
    }
    if (r.type != 'C' || !have_off) continue;
    n_frame++;

    // Skip the metadata row; the estimator config is 640x480.
    if (r.bytes < (uint32_t)r.w * r.h) continue;
    cv::Mat full(r.h, r.w, CV_8UC1, buf.data());
    cv::Mat img = full.rowRange(1, r.h).clone();
    // Pair this frame with the most recent exposure stamp to sample the clock offset. Both run at
    // 30 Hz, so "most recent" is right to within one frame; the median absorbs the rest.
    if (!expo.empty()) {
      off_samples.push_back((double)r.dev_ns - (double)expo.back());
      if (off_samples.size() > 120) off_samples.pop_front();
      if (off_samples.size() >= 30) {
        std::vector<double> v(off_samples.begin(), off_samples.end());
        std::nth_element(v.begin(), v.begin() + v.size() / 2, v.end());
        cam_off = v[v.size() / 2];
        if (!have_camoff) {
          have_camoff = true;
          fprintf(stderr, "[+] camera->IMU clock offset = %.3f s (V4L2 monotonic vs nRF)\n",
                  cam_off * 1e-9);
        }
      }
    }
    if (!have_camoff) continue;
    double t = ((double)r.dev_ns - cam_off) * 1e-9;
    int idx = r.cam & 3;
    pend[idx] = img; pend_t[idx] = t; pend_ok[idx] = true;

    // cam0 is the reference; cam2 is the stereo partner (best baseline, notes/15). Feed when both
    // are present and within half a frame.
    if (!(pend_ok[0] && pend_ok[2])) continue;
    if (std::fabs(pend_t[0] - pend_t[2]) > 0.016) continue;

    ov_core::CameraData cam;
    cam.timestamp = pend_t[0];
    cam.sensor_ids.push_back(0); cam.images.push_back(pend[0]);
    cam.masks.push_back(cv::Mat::zeros(pend[0].rows, pend[0].cols, CV_8UC1));
    cam.sensor_ids.push_back(1); cam.images.push_back(pend[2]);
    cam.masks.push_back(cv::Mat::zeros(pend[2].rows, pend[2].cols, CV_8UC1));
    pend_ok[0] = pend_ok[2] = false;
    n_pair++;

    lead_ms.push_back((t_last_imu - cam.timestamp) * 1e3);
    struct timespec a, b;
    clock_gettime(CLOCK_MONOTONIC, &a);
    sys->feed_measurement_camera(cam);
    clock_gettime(CLOCK_MONOTONIC, &b);
    proc_ms.push_back((b.tv_sec - a.tv_sec) * 1e3 + (b.tv_nsec - a.tv_nsec) * 1e-6);

    if (!sys->initialized()) continue;
    auto st = sys->get_state();
    Eigen::Matrix<double, 4, 1> q = st->_imu->quat();   // JPL x,y,z,w
    Eigen::Matrix<double, 3, 1> p = st->_imu->pos();
    if (t_first_pose < 0) {
      t_first_pose = st->_timestamp;
      fprintf(stderr, "[+] initialised after %ld frames / %ld imu\n", n_pair, n_imu);
    }
    t_last_pose = st->_timestamp;

    if (do_inject) {
      float fp[3] = {(float)p(0), (float)p(1), (float)p(2)};
      float fq[4] = {(float)q(0), (float)q(1), (float)q(2), (float)q(3)};   // xyzw on the wire
      bool ok = inject_field(0, fp, 3) && inject_field(1, fq, 4);
      if (ok) n_inj++; else n_injfail++;
    }
    if (n_pair % 150 == 0)
      fprintf(stderr, "[.] pairs=%ld imu=%ld injected=%ld p=(%.3f %.3f %.3f)\n",
              n_pair, n_imu, n_inj, p(0), p(1), p(2));
  }

  double med = 0;
  if (!proc_ms.empty()) {
    std::sort(proc_ms.begin(), proc_ms.end());
    med = proc_ms[proc_ms.size() / 2];
  }
  // How constant is the V4L2<->exposure offset really? A constant offset is only valid if this is
  // tight; if it is noisy, every frame carries timing error that per-frame snapping would not have.
  fprintf(stderr, "[+] IMU out-of-order: %ld/%ld (%.2f%%), older-than-last-frame: %ld\n",
          n_imu_ooo, n_imu, 100.0 * n_imu_ooo / (n_imu ? n_imu : 1), n_imu_stale);
  if (off_samples.size() > 10) {
    std::vector<double> v(off_samples.begin(), off_samples.end());
    std::sort(v.begin(), v.end());
    fprintf(stderr, "[+] V4L2-exposure offset spread (ms): p10=%.2f median=%.2f p90=%.2f range=%.2f\n",
            (v[v.size()/10]-v[v.size()/2])*1e-6, 0.0,
            (v[9*v.size()/10]-v[v.size()/2])*1e-6, (v.back()-v.front())*1e-6);
  }
  if (!lead_ms.empty()) {
    std::vector<double> L = lead_ms;
    std::sort(L.begin(), L.end());
    long neg = 0; for (double x : lead_ms) if (x < 0) neg++;
    fprintf(stderr, "[+] IMU lead at camera feed (ms): p10=%.1f median=%.1f p90=%.1f  negative on %ld/%zu (%.1f%%)\n",
            L[L.size()/10], L[L.size()/2], L[9*L.size()/10], neg, L.size(), 100.0*neg/L.size());
  }
  fprintf(stderr,
          "\n[+] imu=%ld expo=%ld frames=%ld pairs=%ld injected=%ld failed=%ld\n"
          "[+] estimator median %.2f ms/frame (p90 %.2f), tracked %.2f s\n",
          n_imu, n_expo, n_frame, n_pair, n_inj, n_injfail, med,
          proc_ms.empty() ? 0.0 : proc_ms[(size_t)(0.9 * proc_ms.size())],
          (t_first_pose < 0) ? 0.0 : t_last_pose - t_first_pose);
  return 0;
}
