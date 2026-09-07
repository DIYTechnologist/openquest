// bench_runner.cpp — run OpenVINS on a EuRoC dataset and report per-frame cost. Step X of notes/18.
//
// The question this answers: does OpenVINS fit the 33.3 ms budget at 30 Hz on the Quest's own
// Snapdragon 835? Everything to date has run on the host. Step 4 (replacing trackingservice in
// place) is impossible if the answer is no, so this is measured before building toward it.
//
// Feeding logic is deliberately identical to tools/openvins-docker/euroc_runner.cpp, including the
// IMU_LEAD_S look-ahead, so the on-device numbers describe the same work the host does. What is
// added is timing: wall-clock per camera message, plus the CPU clocks at intervals, because a
// median that looks fine while the SoC is thermally throttling is not a usable answer.
//
// Usage: ov_bench <config.yaml> <dataset_dir> [max_frames]

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <numeric>
#include <vector>

#include <opencv2/opencv.hpp>
#include <time.h>
#include <unistd.h>

#include "core/VioManager.h"
#include "core/VioManagerOptions.h"
#include "state/State.h"
#include "utils/print.h"
#include "utils/sensor_data.h"

using namespace ov_msckf;

static constexpr double IMU_LEAD_S = 0.10;

struct ImuRow { double t; Eigen::Vector3d w, a; };
struct CamRow { double t; std::string file; };

static std::vector<std::string> split(const std::string &s, char d) {
  std::vector<std::string> o; std::stringstream ss(s); std::string it;
  while (std::getline(ss, it, d)) o.push_back(it);
  return o;
}

static double now_s() {
  struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec + ts.tv_nsec * 1e-9;
}

// Reading the scaling governor's current frequency is the cheapest way to see throttling. A run
// whose median is inside budget only because it finished before the SoC heated up is not evidence.
static void dump_clocks(const char *tag) {
  printf("[clocks %s]", tag);
  for (int c = 0; c < 8; c++) {
    char p[128];
    snprintf(p, sizeof p, "/sys/devices/system/cpu/cpu%d/cpufreq/scaling_cur_freq", c);
    FILE *f = fopen(p, "r");
    if (!f) continue;
    long khz = 0; if (fscanf(f, "%ld", &khz) == 1) printf(" cpu%d=%ldMHz", c, khz / 1000);
    fclose(f);
  }
  printf("\n"); fflush(stdout);
}

int main(int argc, char **argv) {
  if (argc < 3) { fprintf(stderr, "usage: ov_bench <config.yaml> <dataset> [max_frames]\n"); return 1; }
  std::string cfg = argv[1], ds = argv[2];
  size_t maxf = (argc > 3) ? (size_t)atoi(argv[3]) : SIZE_MAX;

  auto parser = std::make_shared<ov_core::YamlParser>(cfg);
  VioManagerOptions params;
  params.print_and_load(parser);
  params.num_opencv_threads = 4;
  { std::string v = "SILENT"; ov_core::Printer::setPrintLevel(v); }   // logging would distort timing
  auto sys = std::make_shared<VioManager>(params);

  std::vector<ImuRow> imu; std::vector<CamRow> c0, c1;
  { std::ifstream f(ds + "/mav0/imu0/data.csv"); std::string l;
    while (std::getline(f, l)) { if (l.empty() || l[0] == '#') continue; auto c = split(l, ',');
      if (c.size() < 7) continue; ImuRow r; r.t = std::stod(c[0]) * 1e-9;
      r.w << std::stod(c[1]), std::stod(c[2]), std::stod(c[3]);
      r.a << std::stod(c[4]), std::stod(c[5]), std::stod(c[6]); imu.push_back(r); } }
  auto loadcam = [&](const std::string &p, std::vector<CamRow> &v) {
    std::ifstream f(p); std::string l;
    while (std::getline(f, l)) { if (l.empty() || l[0] == '#') continue; auto c = split(l, ',');
      if (c.size() < 2) continue; v.push_back({std::stod(c[0]) * 1e-9, c[1]}); } };
  loadcam(ds + "/mav0/cam0/data.csv", c0);
  loadcam(ds + "/mav0/cam1/data.csv", c1);
  printf("loaded imu=%zu cam0=%zu cam1=%zu\n", imu.size(), c0.size(), c1.size());
  if (imu.empty() || c0.empty()) return 1;

  bool stereo = params.state_options.num_cameras > 1;
  std::vector<double> t_total, t_read;
  size_t iidx = 0, n1 = 0, nframes = std::min(maxf, c0.size());
  dump_clocks("start");
  double wall0 = now_s();

  for (size_t k = 0; k < nframes; k++) {
    while (iidx < imu.size() && imu[iidx].t <= c0[k].t + IMU_LEAD_S) {
      ov_core::ImuData m; m.timestamp = imu[iidx].t; m.wm = imu[iidx].w; m.am = imu[iidx].a;
      sys->feed_measurement_imu(m); iidx++;
    }
    // Image decode is timed separately: on the real device frames arrive as raw buffers from the
    // camera, so PNG decode is an artefact of the dataset and must not be charged to the estimator.
    double tr0 = now_s();
    cv::Mat i0 = cv::imread(ds + "/mav0/cam0/data/" + c0[k].file, cv::IMREAD_GRAYSCALE);
    if (i0.empty()) continue;
    ov_core::CameraData cam;
    cam.timestamp = c0[k].t;
    cam.sensor_ids.push_back(0); cam.images.push_back(i0);
    cam.masks.push_back(cv::Mat::zeros(i0.rows, i0.cols, CV_8UC1));
    if (stereo && n1 < c1.size()) {
      while (n1 + 1 < c1.size() && c1[n1].t < c0[k].t - 1e-6) n1++;
      if (std::abs(c1[n1].t - c0[k].t) < 1e-6) {
        cv::Mat i1 = cv::imread(ds + "/mav0/cam1/data/" + c1[n1].file, cv::IMREAD_GRAYSCALE);
        if (!i1.empty()) { cam.sensor_ids.push_back(1); cam.images.push_back(i1);
                           cam.masks.push_back(cv::Mat::zeros(i1.rows, i1.cols, CV_8UC1)); }
      }
    }
    double tr1 = now_s();
    sys->feed_measurement_camera(cam);
    double tr2 = now_s();
    t_read.push_back((tr1 - tr0) * 1e3);
    t_total.push_back((tr2 - tr1) * 1e3);
    if (k % 200 == 0 && k) { printf("frame %zu/%zu init=%d\n", k, nframes, (int)sys->initialized());
                            dump_clocks("mid"); }
  }
  double wall = now_s() - wall0;
  dump_clocks("end");

  auto pct = [](std::vector<double> v, double p) {
    if (v.empty()) return 0.0; std::sort(v.begin(), v.end());
    return v[std::min(v.size() - 1, (size_t)(p * v.size()))]; };

  printf("\n=== step X: OpenVINS on-device, %s ===\n", stereo ? "stereo" : "mono");
  printf("frames processed : %zu over %.1f s wall\n", t_total.size(), wall);
  printf("BUDGET at 30 Hz  : 33.30 ms/frame\n");
  printf("estimator ms/frame: median %.2f  mean %.2f  p90 %.2f  p99 %.2f  max %.2f\n",
         pct(t_total, 0.5), std::accumulate(t_total.begin(), t_total.end(), 0.0) / t_total.size(),
         pct(t_total, 0.9), pct(t_total, 0.99), pct(t_total, 1.0));
  printf("png decode ms/frame (dataset artefact, NOT charged): median %.2f\n", pct(t_read, 0.5));
  printf("VERDICT: median is %.2fx the 30 Hz budget\n", pct(t_total, 0.5) / 33.3);
  return 0;
}
