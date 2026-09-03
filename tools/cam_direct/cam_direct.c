// cam_direct.c — B1 bring-up: drive the 4 tracking cameras from OUR OWN process,
// with trackingservice and the sensors HAL stopped. No HIDL, no ISensorClient.
//
// We dlopen Meta's thin vendor camera stack and call its plain-C API directly:
//   libqcameraoculushal.so   qcamera_*     (this file's target; deps: liblog + the driver lib)
//     -> libqcameradriver.so control_*     (raw v4l2 ioctls on /dev/media*, CSIPHY/CSID/ISPIF/ISP)
//        -> kernel: stock published camera_v2 + the in-kernel `oculus,camera` sensor driver
// Sensor power-up and OV7251 I2C init happen in the KERNEL, so there are no register
// tables to supply from here. See notes/11-camera-kernel-path-probe.md.
//
// Signatures below were recovered by disassembling libqcameraoculushal.so and its call sites
// in libqcamerahal.so. Confidence is marked per function; the UNKNOWNs are why this is staged
// and why stage 2+ dumps raw bytes — one live run should close them.
//
// !! __android_log_assert() ABORTS. The lib asserts on: bad qcamera_open mode, out-of-range
// !! sensor index, and dequeue/get_fd on a sensor that is not started. Stages are ordered so we
// !! never call into a state we know asserts.
//
// !! Even stage 1 touches hardware: qcamera_open -> control_init opens /dev/media*. The sensors
// !! HAL (which holds /dev/video0) MUST be stopped first or we contend with it.
//
// Build:
//   NDK=~/diytech/quest/tools/android-ndk-r27c/toolchains/llvm/prebuilt/linux-x86_64/bin
//   $NDK/aarch64-linux-android29-clang -O2 -o cam_direct cam_direct.c -ldl
// Run (as root, SELinux permissive, HAL + trackingservice stopped):
//   ./cam_direct enum | ./cam_direct start | ./cam_direct stream [nframes]

#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <dlfcn.h>
#include <time.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <pthread.h>
#include <poll.h>
#include <errno.h>

#define OCULUSHAL "/system/vendor/lib64/libqcameraoculushal.so"
#define OUTDIR    "/data/local/tmp/camdirect"

// ── syncboss camera power gate ───────────────────────────────────────────────────────────────
// The OV7251s are FSIN-triggered slaves; nothing exposes them until the nRF is told to power
// them. From the published driver (drivers/staging/oculus/mcu/syncboss/syncboss_spi.c):
// queue_tx_packet() snoops the first byte of any write to /dev/syncboss0 and on packet type 40
// calls syncboss_on_camera_probe() -> "Turning on cameras" -> enable_cameras() (regulators +
// MCLK for all 4 sensors). Type 41 is the release. CONFIG_SYNCBOSS_CAMERA_CONTROL=y here.
// stop_streaming_locked() force-releases the cameras when the last streaming client closes,
// so we must hold /dev/syncboss_stream0 open for as long as we want the cameras powered.
#define SB_DEV      "/dev/syncboss0"
#define SB_STREAM   "/dev/syncboss_stream0"
#define SB_CAM_PROBE   40
#define SB_CAM_RELEASE 41

static int sb_stream_fd = -1, sb_fd = -1;

static int syncboss_cam_power(int on) {
  unsigned char pkt[3] = { on ? SB_CAM_PROBE : SB_CAM_RELEASE, 0, 0 };  // {type, seq, data_len}
  if (on) {
    // Hold the stream open: stop_streaming_locked() force-releases the cameras when the last
    // streaming client goes away.
    sb_stream_fd = open(SB_STREAM, O_RDONLY);
    printf("[%c] open %s -> fd %d\n", sb_stream_fd < 0 ? '-' : '+', SB_STREAM, sb_stream_fd);
    sb_fd = open(SB_DEV, O_RDWR);
    printf("[%c] open %s -> fd %d\n", sb_fd < 0 ? '-' : '+', SB_DEV, sb_fd);
    if (sb_fd < 0) return -1;
  }
  if (sb_fd < 0) return -1;
  ssize_t n = write(sb_fd, pkt, sizeof pkt);
  printf("[%c] raw syncboss %s (type %u) -> write %zd\n",
         n == (ssize_t)sizeof pkt ? '+' : '-', on ? "CAMERA_PROBE" : "CAMERA_RELEASE", pkt[0], n);
  if (!on) { if (sb_fd >= 0) close(sb_fd); if (sb_stream_fd >= 0) close(sb_stream_fd); }
  return n == (ssize_t)sizeof pkt ? 0 : -1;
}

// ── Step 1.2 (notes/18): the MCU command set, WITHOUT libsyncboss.so ─────────────────────────
//
// libsyncboss.so is one of the three closed Meta blobs the OS swap will delete (notes/16), so the
// camera path cannot depend on it. Its whole job is marshalling a handful of packets onto
// /dev/syncboss0, and the packets are the real ABI — the kernel only snoops types 40/41 to gate
// camera power (syncboss_spi.c:113) and passes everything else through to the MCU untouched.
//
// The formats below were not guessed: they were read off the wire by tracing writes during a
// working libsyncboss session (tools/cam_kernel, notes/19). Each libsyncboss call maps 1:1 onto
// one packet, and the traced payloads carry back the exact arguments we passed in — e.g.
// set_exposure_gain(3000,160) appeared as `2a 00 10 b80b b80b b80b b80b a000 a000 a000 a000`,
// with 0x0bb8 = 3000 and 0x00a0 = 160.
//
// Wire format is uniform: { u8 type, u8 seq, u8 data_len, u8 data[data_len] }.
// Camera-control ops (0x28/0x2a/0x2c/0x2e) go out with seq 0 and are fire-and-forget; the generic
// property op 0x03 uses an incrementing seq because the MCU replies to it.
#define SB_CAM_INIT      0x2e   // camera_init(num_cams)
#define SB_CAM_START     0x2c   // start_streaming(num_cams)  <-- FSIN strobe
#define SB_CAM_STOP      0x2d   // stop_streaming
#define SB_CAM_DEINIT    0x2f   // camera_deinit
#define SB_CAM_EXPGAIN   0x2a   // set_exposure_gain(u16 exp[4], u16 gain[4])
#define SB_PROP          0x03   // generic property set, first data byte selects the property
#define SB_PROP_BPP      0x8c   // set_bpp(n)
#define SB_PROP_PERIOD   0x89   // set_frame_rate(u32 period_us)
#define SB_PROP_TAGMODE  0x8d   // set_frame_tag_mode(n)

static unsigned char sb_seq = 0xa0;   // property ops expect a reply; keep the seq moving

static int sb_send(const char *what, unsigned char type, unsigned char seq,
                   const void *data, unsigned char len) {
  unsigned char pkt[64];
  if (len > sizeof pkt - 3) return -1;
  pkt[0] = type; pkt[1] = seq; pkt[2] = len;
  if (len) memcpy(pkt + 3, data, len);
  ssize_t n = write(sb_fd, pkt, (size_t)len + 3);
  printf("[%c] raw %-22s type=0x%02x len=%u -> %zd\n",
         n == (ssize_t)len + 3 ? '+' : '-', what, type, len, n);
  return n == (ssize_t)len + 3 ? 0 : -1;
}

static int sb_prop(const char *what, unsigned char prop, const void *val, unsigned char vlen) {
  unsigned char d[8];
  d[0] = prop;
  memcpy(d + 1, val, vlen);
  int rc = sb_send(what, SB_PROP, sb_seq++, d, (unsigned char)(vlen + 1));
  usleep(20000);            // the MCU answers these; give it the same slack libsyncboss allowed
  return rc;
}

// Mirrors syncboss_lib_start() + syncboss_lib_start_streaming(), packet for packet.
static int sb_raw_setup(int num_cams, uint16_t exposure, uint16_t gain) {
  unsigned char n8 = (unsigned char)num_cams, bpp = 8, tag = 1;
  uint32_t period = 33333;                          // microseconds, not Hz (notes/13)
  int rc = 0;
  rc |= sb_prop("set_bpp(8)", SB_PROP_BPP, &bpp, 1);
  rc |= sb_send("camera_init", SB_CAM_INIT, 0, &n8, 1);
  rc |= sb_prop("set_frame_rate(33333us)", SB_PROP_PERIOD, &period, 4);
  if (exposure) {
    uint16_t eg[8] = { exposure, exposure, exposure, exposure, gain, gain, gain, gain };
    rc |= sb_send("set_exposure_gain", SB_CAM_EXPGAIN, 0, eg, sizeof eg);
  }
  rc |= sb_prop("set_frame_tag_mode(1)", SB_PROP_TAGMODE, &tag, 1);
  return rc;
}

// Must run AFTER the v4l2 pipeline is up, so the MCU strobes into a listening receiver.
static int sb_raw_start_streaming(int num_cams) {
  unsigned char n8 = (unsigned char)num_cams;
  return sb_send("start_streaming  <-- FSIN", SB_CAM_START, 0, &n8, 1);
}

static void sb_raw_stop(void) {
  unsigned char x = 0x98;                            // value libsyncboss sends for both
  sb_send("stop_streaming", SB_CAM_STOP, sb_seq++, &x, 1);
  sb_send("camera_deinit",  SB_CAM_DEINIT, sb_seq++, &x, 1);
}

// ── libsyncboss.so: the MCU command API the HAL itself uses ──────────────────────────────────
// SensorTraits<Imu>::enableSensor is just a wrapper around syncboss_imu_enable@plt, so the whole
// MCU command set is this plain-C library (deps: liblog/libm/libdl/libc). Camera probe alone only
// powers rails+MCLK; syncboss_camera_start_streaming (MCU message type 44) is what makes the MCU
// strobe FSIN, which is what the OV7251 slaves need in order to expose.
#define SYNCBOSS_LIB "/system/vendor/lib64/libsyncboss.so"

static uint16_t g_exposure = 300, g_gain = 100;   // per-camera exposure/gain, overridable from argv

static struct {
  void *lib;
  int (*init)(void **out_handle, const void *cfg); // writes the handle through arg0 (str x19,[x20])
  int (*deinit)(void *handle);
  int (*cam_probe)(void *handle, unsigned char *out);
  int (*cam_init)(void *handle, unsigned char a);
  int (*cam_set_bpp)(void *handle, unsigned char bpp);
  int (*cam_set_frame_rate)(void *handle, uint32_t period);  // u32 payload; HAL calls this from applyFramePeriod() => a PERIOD, not an fps
  int (*cam_set_frame_tag_mode)(void *handle, unsigned char mode);
  int (*cam_start)(void *handle, unsigned char num_cams);  // arg is a COUNT at the HAL call site
  // set_exposure_gain(h, const u16 exp[4], const u16 gain[4], u8 count) — count is asserted ==4,
  // message type 42, 16-byte payload (4 exposures + 4 gains, both u16).
  int (*cam_set_exposure_gain)(void *handle, const uint16_t *exp, const uint16_t *gain, unsigned char n);
  int (*imu_enable)(void *handle);     // handle only; msg type 110, response 125
  int (*cam_stop)(void *handle);
  int (*cam_release)(void *handle);
  int (*cam_deinit)(void *handle);
} sb;

// syncboss_init ALLOCATES the handle and writes the pointer through arg0 (`str x19, [x20]`).
// Passing a buffer directly and using it as the handle left +0x8 NULL and crashed
// syncboss_camera_probe+20, which does `ldr x0, [x0, #8]` to take a lock.
static void *sb_handle;
static int sb_lib_ready;
// Step 1.2: SYNCBOSS_RAW=1 drives the MCU with our own packets and never dlopens the blob,
// so the two paths can be A/B'd on the same hardware in the same session.
static int sb_raw(void) { const char *e = getenv("SYNCBOSS_RAW"); return e && *e != '0'; }

static int syncboss_lib_start(int num_cams, int fps) {
  if (sb_raw()) { (void)fps; return sb_raw_setup(num_cams, g_exposure, g_gain); }
  sb.lib = dlopen(SYNCBOSS_LIB, RTLD_NOW);
  if (!sb.lib) { printf("[-] dlopen %s: %s\n", SYNCBOSS_LIB, dlerror()); return -1; }
  printf("[+] dlopen %s -> %p\n", SYNCBOSS_LIB, sb.lib);
  sb.init               = dlsym(sb.lib, "syncboss_init");
  sb.deinit             = dlsym(sb.lib, "syncboss_deinit");
  sb.cam_probe          = dlsym(sb.lib, "syncboss_camera_probe");
  sb.cam_init           = dlsym(sb.lib, "syncboss_camera_init");
  sb.cam_set_bpp        = dlsym(sb.lib, "syncboss_camera_set_bpp");
  sb.cam_set_frame_rate = dlsym(sb.lib, "syncboss_camera_set_frame_rate");
  sb.cam_set_frame_tag_mode = dlsym(sb.lib, "syncboss_camera_set_frame_tag_mode");
  sb.cam_start          = dlsym(sb.lib, "syncboss_camera_start_streaming");
  sb.cam_set_exposure_gain = dlsym(sb.lib, "syncboss_camera_set_exposure_gain");
  sb.imu_enable         = dlsym(sb.lib, "syncboss_imu_enable");
  sb.cam_stop           = dlsym(sb.lib, "syncboss_camera_stop_streaming");
  sb.cam_release        = dlsym(sb.lib, "syncboss_camera_release");
  sb.cam_deinit         = dlsym(sb.lib, "syncboss_camera_deinit");
  if (!sb.init || !sb.cam_start) { printf("[-] missing syncboss symbols\n"); return -1; }

  int rc = sb.init(&sb_handle, NULL);
  printf("[%c] syncboss_init(&handle, NULL) rc=%d handle=%p\n", rc ? '-' : '+', rc, sb_handle);
  if (rc || !sb_handle) return -1;
  sb_lib_ready = 1;

  unsigned char probe_out = 0xff;
  rc = sb.cam_probe(sb_handle, &probe_out);
  printf("[%c] syncboss_camera_probe rc=%d out=0x%02x\n", rc ? '-' : '+', rc, probe_out);

  if (sb.cam_set_bpp) printf("[*] set_bpp(8) rc=%d\n", sb.cam_set_bpp(sb_handle, 8));
  if (sb.cam_init)    printf("[*] camera_init(%d) rc=%d\n", num_cams,
                             sb.cam_init(sb_handle, (unsigned char)num_cams));
  (void)fps;
  return 0;
}

// Second phase, mirroring MontereyCameraProvider::startCameras(): this must run AFTER the v4l2
// pipeline is streaming, so the MCU strobes into a receiver that is already listening.
static int syncboss_lib_start_streaming(int num_cams) {
  if (sb_raw()) return sb_raw_start_streaming(num_cams);
  if (!sb_lib_ready) return -1;
  // applyFramePeriod() passes a stored u32; 30 was rejected (rc=-1) because this is a PERIOD.
  // Probe a few encodings and keep the first the MCU accepts.
  static const uint32_t cand[] = { 33333, 33, 30, 16666, 66666 };
  if (sb.cam_set_frame_rate) {
    for (unsigned i = 0; i < sizeof cand / sizeof *cand; i++) {
      int r = sb.cam_set_frame_rate(sb_handle, cand[i]);
      printf("[%c] set_frame_rate(%u) rc=%d\n", r ? '-' : '+', cand[i], r);
      if (r == 0) break;
    }
  }
  if (sb.cam_set_exposure_gain && g_exposure) {   // exposure 0 => leave the MCU defaults alone
    // Room frames came out very dark (mean ~10) at whatever the MCU defaults are, so set these
    // explicitly. Both arrays are per-camera (4 entries each).
    uint16_t exp[4]  = { g_exposure, g_exposure, g_exposure, g_exposure };
    uint16_t gain[4] = { g_gain, g_gain, g_gain, g_gain };
    int r = sb.cam_set_exposure_gain(sb_handle, exp, gain, 4);
    printf("[%c] set_exposure_gain(exp=%u gain=%u x4) rc=%d\n", r ? '-' : '+', g_exposure, g_gain, r);
  }
  if (sb.cam_set_frame_tag_mode) printf("[*] set_frame_tag_mode(1) rc=%d\n",
                                        sb.cam_set_frame_tag_mode(sb_handle, 1));
  int rc = sb.cam_start(sb_handle, (unsigned char)num_cams);
  printf("[%c] syncboss_camera_start_streaming(%d) rc=%d  <-- FSIN strobe\n",
         rc ? '-' : '+', num_cams, rc);
  return rc;
}

static void syncboss_lib_stop(void) {
  if (sb_raw()) { sb_raw_stop(); return; }
  if (!sb_lib_ready) return;
  if (sb.cam_stop)    printf("[*] camera_stop_streaming rc=%d\n", sb.cam_stop(sb_handle));
  if (sb.cam_release) printf("[*] camera_release rc=%d\n", sb.cam_release(sb_handle));
  if (sb.cam_deinit)  printf("[*] camera_deinit rc=%d\n", sb.cam_deinit(sb_handle));
  if (sb.deinit)      printf("[*] syncboss_deinit rc=%d\n", sb.deinit(sb_handle));
  sb_lib_ready = 0;
}

// ── Recovered API ────────────────────────────────────────────────────────────────────────────
// CONFIRMED: read directly out of the disassembly, unambiguous.
// INFERRED : consistent with the disassembly + a real call site in libqcamerahal.so.
// UNKNOWN  : placeholder; stage 2 exists to pin it down.

// CONFIRMED. mode is asserted to be 0 or 1. libqcamerahal picks it from a property:
// property value 8 -> qcamera_open(0), value 10 -> qcamera_open(1) => bit depth. We want 8-bit
// mono, which matches the 640x481 mono8 frames the LD_PRELOAD tap already captured.
// Returns a calloc'd 112-byte hal context (num_sensors at +0x48, sensor slots from +0x50).
typedef void *(*fn_open)(int mode);
typedef int   (*fn_close)(void *hal);                       // INFERRED
// CONFIRMED: no handle argument, just wraps control_get_num_of_cameras() and masks to u8.
typedef int   (*fn_num_sensors)(void);
// CONFIRMED: validates 0 <= idx < num_cameras (asserts otherwise), returns a per-sensor object.
// Sensor object: +0x08 = u32 camera index, +0x0c = u32 started flag, +0x10 = {u32 w, u32 h}.
typedef void *(*fn_get_sensor)(void *hal, int idx);
// CONFIRMED: literally `add x0, x0, #0x10; ret` -> &{u32 w, u32 h} inside the sensor object.
typedef void *(*fn_get_sensor_dim)(void *sensor);
// CONFIRMED (corrected after run 1): the FIRST arg is the BIT DEPTH, not a camera index.
// control_get_capability(&cap, bpp) compares bpp against 8 and 10 and asserts otherwise:
//   bpp=8  -> fourcc 0x59455247 "GREY" (V4L2_PIX_FMT_GREY, 8-bit mono), tag 42, k=1, sel=0
//   bpp=10 -> fourcc 0x20303159 "Y10 " (V4L2_PIX_FMT_Y10),             tag 43, k=2, sel=1
// Then *w = cap.w, *h = cap.h + (meta_row&1) -> the extra 481st row. Passing a camera index
// here is what aborted run 1 with "Invalid BPP configuration: 0".
typedef int   (*fn_query_buffer_dimensions)(int bpp, int meta_row, uint32_t *w, uint32_t *h);
// INFERRED: writes the camera index at out+0x24, so `out` is >= 0x28 bytes. Rest unmapped.
typedef int   (*fn_query_sensor_info)(void *sensor, void *out);
// CONFIRMED: asserts sensor+0xc (started) then returns control_get_fd(cam). Post-start only.
typedef int   (*fn_get_fd)(void *sensor);
// PARTIAL. 6 args. From the shared helper and libqcamerahal's StartCamera call site:
//   sensor  - the per-sensor object
//   dim     - ptr to {u32 w, u32 h}; copied to sensor+0x10
//   cfg     - ptr to a 28-byte stream/format config; copied to sensor+0x28..0x44
//   nbufs   - at the call site this is (vec_end - vec_begin)/16, i.e. a count of 16-byte entries
//   p4      - UNKNOWN pointer (buffer descriptors? a callback?) <-- the main open question
//   meta_row- same flag as query_buffer_dimensions; adds the extra row
typedef int   (*fn_start_sensor)(void *sensor, const void *dim, const void *cfg,
                                 int nbufs, void *p4, int meta_row);
typedef int   (*fn_stop_sensor)(void *sensor);              // INFERRED
typedef int   (*fn_release_sensor)(void *sensor);           // INFERRED
// CONFIRMED shape: calls control_dqbuf(cam, arg) and wraps the result in an 80-byte container,
// returning container+8. NULL on failure. The second arg's meaning is UNKNOWN (timeout? stream?).
typedef void *(*fn_dequeue)(void *sensor, int arg);
typedef void *(*fn_dequeue_nb)(void *sensor);               // CONFIRMED: no second arg.
// CONFIRMED: takes the pointer dequeue returned, re-derives container = frame-8, calls
// control_qbuf(cam, container[0]), poisons +0x48 with 0xa5a5a5a5 and frees the container.
typedef int   (*fn_enqueue)(void *sensor, void *frame);

static struct {
  void *lib;
  fn_open open; fn_close close; fn_num_sensors num_sensors;
  fn_get_sensor get_sensor; fn_get_sensor_dim get_sensor_dim;
  fn_query_buffer_dimensions query_dims; fn_query_sensor_info query_info;
  fn_get_fd get_fd; fn_start_sensor start; fn_stop_sensor stop;
  fn_release_sensor release; fn_dequeue dequeue; fn_dequeue_nb dequeue_nb; fn_enqueue enqueue;
} q;

static void *g_sensor;
static int   g_started;   // qcamera_stop_sensor asserts ("Sensor already stopped") if not started   // the one acquired sensor shared by stages 2/3 and cleanup

static uint64_t mono_ns(void) {
  struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
  return (uint64_t)t.tv_sec * 1000000000ull + t.tv_nsec;
}

// Peek at a possibly-invalid address in a forked child, so a bad pointer costs us a child
// process rather than the whole capture.
static int safe_peek(const void *addr, void *out, size_t n) {
  int fds[2];
  if (pipe(fds)) return -1;
  pid_t pid = fork();
  if (pid == 0) { close(fds[0]); ssize_t w = write(fds[1], addr, n); _exit(w == (ssize_t)n ? 0 : 1); }
  close(fds[1]);
  ssize_t got = read(fds[0], out, n);
  close(fds[0]);
  int st; waitpid(pid, &st, 0);
  return got == (ssize_t)n ? 0 : -1;
}

static void hexdump(const char *tag, const void *p, size_t n) {
  const unsigned char *b = p;
  printf("%s (%p, %zu bytes):\n", tag, p, n);
  for (size_t i = 0; i < n; i += 16) {
    printf("  +0x%02zx ", i);
    for (size_t j = 0; j < 16; j++) printf(j + i < n ? "%02x " : "   ", b[i + j]);
    printf(" |");
    for (size_t j = 0; j < 16 && i + j < n; j++)
      putchar(b[i + j] >= 32 && b[i + j] < 127 ? b[i + j] : '.');
    printf("|\n");
  }
  // u64 view — makes pointers, dims and timestamps pop out of the noise
  for (size_t i = 0; i + 8 <= n; i += 8) {
    uint64_t v; memcpy(&v, b + i, 8);
    if (v) printf("  w%-2zu +0x%02zx = 0x%016llx  (%llu)\n", i / 8, i,
                  (unsigned long long)v, (unsigned long long)v);
  }
}

#define RESOLVE(field, name) do {                                              \
    q.field = (void *)dlsym(q.lib, name);                                      \
    if (!q.field) { fprintf(stderr, "dlsym %s failed: %s\n", name, dlerror()); return -1; } \
  } while (0)

static int load_vendor_stack(void) {
  // RTLD_NOW so any missing dependency surfaces here rather than mid-call.
  q.lib = dlopen(OCULUSHAL, RTLD_NOW);
  if (!q.lib) { fprintf(stderr, "dlopen %s failed: %s\n", OCULUSHAL, dlerror()); return -1; }
  printf("[+] dlopen %s -> %p\n", OCULUSHAL, q.lib);
  RESOLVE(open,           "qcamera_open");
  RESOLVE(close,          "qcamera_close");
  RESOLVE(num_sensors,    "qcamera_num_sensors");
  RESOLVE(get_sensor,     "qcamera_get_sensor");
  RESOLVE(get_sensor_dim, "qcamera_get_sensor_dim");
  RESOLVE(query_dims,     "qcamera_query_buffer_dimensions");
  RESOLVE(query_info,     "qcamera_query_sensor_info");
  RESOLVE(get_fd,         "qcamera_get_fd");
  RESOLVE(start,          "qcamera_start_sensor");
  RESOLVE(stop,           "qcamera_stop_sensor");
  RESOLVE(release,        "qcamera_release_sensor");
  RESOLVE(dequeue,        "qcamera_dequeue");
  RESOLVE(dequeue_nb,     "qcamera_dequeue_nonblocking");
  RESOLVE(enqueue,        "qcamera_enqueue");
  printf("[+] all 14 qcamera_* symbols resolved\n");
  return 0;
}

// ── Stage 1: enumeration ─────────────────────────────────────────────────────────────────────
// Everything here is on the confirmed-signature path. Success criteria: 4 sensors, 640x481.
static void *stage_enum(int *out_n) {
  printf("\n=== STAGE 1: enumerate ===\n");

  void *hal = q.open(0);                       // 0 = 8-bit mono
  printf("[+] qcamera_open(0) -> %p\n", hal);
  if (!hal) { fprintf(stderr, "[-] open returned NULL\n"); return NULL; }
  hexdump("hal context", hal, 112);

  int n = q.num_sensors();
  printf("[+] qcamera_num_sensors() = %d  (expect 4)\n", n);
  *out_n = n;

  for (int i = 0; i < n; i++) {
    void *s = q.get_sensor(hal, i);
    printf("\n[+] qcamera_get_sensor(hal, %d) -> %p\n", i, s);
    if (!s) continue;
    hexdump("sensor object", s, 0x80);

    uint32_t *dim = q.get_sensor_dim(s);
    printf("  qcamera_get_sensor_dim -> %p  {w=%u h=%u}\n", (void *)dim, dim[0], dim[1]);

    // The sensor object caches a capability pointer at +0x18 (qcamera_query_sensor_info reads
    // it). Dump it — the 28-byte cfg that start_sensor wants very likely comes from here.
    void *cap = *(void **)((unsigned char *)s + 0x18);
    printf("  sensor+0x18 (capability?) = %p\n", cap);
    if (cap) hexdump("  capability", cap, 0x60);

    unsigned char info[64] = {0};
    int rc = q.query_info(s, info);            // before query_dims: it aborted run 1
    printf("  qcamera_query_sensor_info rc=%d\n", rc);
    hexdump("  sensor info", info, sizeof info);

    uint32_t w = 0, h = 0;
    rc = q.query_dims(8, 1, &w, &h);           // bpp=8 (GREY), meta_row=1 -> expect 640x481
    printf("  qcamera_query_buffer_dimensions(bpp=8, meta_row=1) rc=%d -> %ux%u\n", rc, w, h);
    rc = q.query_dims(8, 0, &w, &h);           // expect 640x480
    printf("  qcamera_query_buffer_dimensions(bpp=8, meta_row=0) rc=%d -> %ux%u\n", rc, w, h);
    // NOTE: qcamera_get_fd asserts (and aborts) unless the sensor is started — not called here.

    // get_sensor is an ACQUIRE, not a getter: a second call for the same index returns NULL
    // until the sensor is released. Hand it back so the later stages can acquire it.
    printf("  qcamera_release_sensor rc=%d\n", q.release(s));
  }
  return hal;
}

// ── Stage 2: start one sensor ────────────────────────────────────────────────────────────────
// The 28-byte cfg and the p4 pointer are the open unknowns. We start from zeroed/NULL and let
// the lib's own asserts and its QCameraOculusHAL logcat lines tell us what it actually wants.
static int stage_start(void *hal, int cam, int variant) {
  printf("\n=== STAGE 2: start sensor %d (cfg variant %d) ===\n", cam, variant);

  void *s = g_sensor ? g_sensor : q.get_sensor(hal, cam);
  if (!s) { fprintf(stderr, "[-] get_sensor(%d) NULL\n", cam); return -1; }
  g_sensor = s;

  uint32_t w = 0, h = 0;
  // meta_row=0 here: start_sensor adds the extra row itself, so passing the 481 value made the
  // sensor come up as 640x482. Ask for the base 640x480 and let start_sensor add the row.
  q.query_dims(8, 0, &w, &h);   // returns void — judge by the out-params, not a return code
  if (!w || !h) { fprintf(stderr, "[-] query_buffer_dimensions gave %ux%u\n", w, h); return -1; }
  uint32_t dim[2] = { w, h };
  unsigned char cfg[28] = {0};
  int nbufs = 4;

  // The 28-byte cfg is the main unknown. Try progressively better-informed guesses; each runs
  // in its own process so an abort inside the vendor lib doesn't lose the other variants.
  switch (variant) {
    case 0:                                    // zeroed
      break;
    case 1: {                                  // straight from the cached capability struct
      void *cap = *(void **)((unsigned char *)s + 0x18);
      if (cap) memcpy(cfg, cap, sizeof cfg);
      break;
    }
    case 2: {                                  // from qcamera_query_sensor_info's output
      unsigned char info[64] = {0};
      q.query_info(s, info);
      memcpy(cfg, info, sizeof cfg);
      break;
    }
    case 3: {                                  // hand-built: w, h, stride, fourcc 'GREY', bpp
      uint32_t *c = (uint32_t *)cfg;
      c[0] = w; c[1] = h; c[2] = w; c[3] = 0x59455247u /* GREY */; c[4] = 8;
      break;
    }
    // "Invalid cam_format 0 for raw stream" => cfg[0] is a QC cam_format_t.
    // mm_stream_calc_offset_raw accepts 2..123, and control_get_capability stamps 42 for
    // 8-bit mono (43 for 10-bit) — so 42 is the mono8 format enum.
    case 4:
      ((uint32_t *)cfg)[0] = 42;
      break;
    case 5: {
      uint32_t *c = (uint32_t *)cfg;
      c[0] = 42; c[1] = w; c[2] = h; c[3] = w; c[4] = 8; c[5] = 1;
      break;
    }
    // Resolved from mm_stream_get_v4l2_fmt's jump table: cam_format 101 and 112 are the only
    // values that map to fourcc 'GREY' (V4L2_PIX_FMT_GREY). 42 passed the range check in
    // mm_stream_calc_offset_raw but had no V4L2 mapping ("Unknown fmt=42"), so the sensor
    // was configured with a bogus pixelformat and never streamed.
    case 7:
      ((uint32_t *)cfg)[0] = 101;
      break;
    case 8:
      ((uint32_t *)cfg)[0] = 112;
      break;
    case 6: {
      unsigned char info[64] = {0};
      q.query_info(s, info);
      memcpy(cfg, info, sizeof cfg);
      ((uint32_t *)cfg)[0] = 42;
      break;
    }
  }
  hexdump("  cfg being passed", cfg, sizeof cfg);

  printf("[*] qcamera_start_sensor(s=%p, dim={%u,%u}, cfg=variant%d, nbufs=%d, p4=NULL, "
         "meta_row=1)\n", s, w, h, variant, nbufs);
  fflush(stdout);                              // in case the call aborts inside the vendor lib

  int rc = q.start(s, dim, cfg, nbufs, NULL, 1);
  printf("[%c] qcamera_start_sensor rc=%d\n", rc == 0 ? '+' : '-', rc);
  g_started = (*(uint32_t *)((unsigned char *)s + 0x0c) != 0);   // sensor+0xc = started flag
  printf("    sensor+0x0c started flag = %u\n", *(uint32_t *)((unsigned char *)s + 0x0c));
  hexdump("sensor object after start", s, 0x80);

  if (rc == 0) {
    printf("[+] qcamera_get_fd -> %d\n", q.get_fd(s));   // safe: started
    printf("\n=== STAGE 2b: syncboss startCameras (after v4l2 is up) ===\n");
    syncboss_lib_start_streaming(4);
    usleep(300000);
  }
  return rc;
}

// ── Stage 3: pump frames ─────────────────────────────────────────────────────────────────────
// Dumps the 80-byte frame container raw so we can locate the pixel pointer and the exposure
// timestamp empirically. Cross-check pixels against exports/ib-capture-2026-09-01/.
static int stage_stream(void *hal, int cam, int nframes) {
  printf("\n=== STAGE 3: stream %d frames from sensor %d ===\n", nframes, cam);

  void *s = g_sensor;
  if (!s) { fprintf(stderr, "[-] no acquired sensor\n"); return -1; }

  for (int i = 0; i < nframes; i++) {
    uint64_t t0 = mono_ns();
    void *frame = NULL;
    // dequeue is non-blocking here (returns in ~0.03 ms), so poll for up to ~3 s: the sensor
    // needs time to start streaming after the syncboss camera probe powers it.
    for (int tries = 0; tries < 60 && !frame; tries++) {
      frame = q.dequeue(s, 0);
      if (!frame) usleep(50000);
    }
    uint64_t t1 = mono_ns();
    printf("\n[frame %d] qcamera_dequeue -> %p  (%.1f ms)\n", i, frame, (t1 - t0) / 1e6);
    if (!frame) { fprintf(stderr, "[-] dequeue returned NULL after ~3 s of polling\n"); break; }

    // dequeue returns container+8; the container is 80 bytes, so dump from -8.
    hexdump("frame container", (unsigned char *)frame - 8, 80);
    // container[0] is the driver buffer struct from control_dqbuf — follow it one level.
    void *drvbuf = *(void **)((unsigned char *)frame - 8);
    if (drvbuf) {
      hexdump("  driver buffer", drvbuf, 0x60);
      // Any u64 in the container/driver buffer that is a valid readable address is a pixel
      // pointer candidate; dump the first bytes of each so we can spot real image data.
      const uint64_t *cand = (const uint64_t *)drvbuf;
      for (int k = 0; k < 12; k++) {
        unsigned char probe[32];
        if (cand[k] > 0x10000 && safe_peek((void *)(uintptr_t)cand[k], probe, sizeof probe) == 0) {
          char tag[64]; snprintf(tag, sizeof tag, "  readable via drvbuf+0x%02x", k * 8);
          hexdump(tag, probe, sizeof probe);
        }
      }
    }

    // Frame layout (recovered live): container+0x38 = page-aligned pixel VA,
    // container+0x08/+0x10 = CLOCK_MONOTONIC sec/ns, drvbuf+0x2c = byte size (640*481).
    unsigned char *c = (unsigned char *)frame - 8;
    void    *px    = *(void **)(c + 0x38);
    uint64_t ts_s  = *(uint64_t *)(c + 0x08);
    uint64_t ts_ns = *(uint64_t *)(c + 0x10);
    uint32_t sz    = drvbuf ? *(uint32_t *)((unsigned char *)drvbuf + 0x2c) : 0;
    printf("  ts=%llu.%09llu  pixels=%p  size=%u\n",
           (unsigned long long)ts_s, (unsigned long long)ts_ns, px, sz);

    if (px && sz && sz <= 4 * 1024 * 1024) {
      unsigned char *buf = malloc(sz);
      if (buf && safe_peek(px, buf, sz) == 0) {
        unsigned long sum = 0; unsigned char mn = 255, mx = 0;
        for (uint32_t k = 0; k < sz; k++) { sum += buf[k]; if (buf[k] < mn) mn = buf[k]; if (buf[k] > mx) mx = buf[k]; }
        printf("  pixel stats: min=%u max=%u mean=%.1f  %s\n", mn, mx, (double)sum / sz,
               mx > mn ? "REAL IMAGE DATA" : "flat/blank");
        mkdir(OUTDIR, 0755);
        char path[128]; snprintf(path, sizeof path, OUTDIR "/cam%d_%03d.gray", cam, i);
        int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd >= 0) { ssize_t wr = write(fd, buf, sz); close(fd);
                       printf("  wrote %s (%zd bytes)\n", path, wr); }
      }
      free(buf);
    }

    int rc = q.enqueue(s, frame);              // must return the buffer or the queue starves
    printf("  qcamera_enqueue rc=%d\n", rc);
  }
  return 0;
}

// ── Stage 4: all four cameras ────────────────────────────────────────────────────────────────
// Starts each sensor individually (the known-good path) rather than qcamera_start_all_sensors.
// Exposure sync across cameras comes from the MCU's FSIN strobe, not from the v4l2 start, so
// per-sensor starts still give hardware-synchronised frames.
static int stage_all(void *hal, int n, int rounds) {
  printf("\n=== STAGE 4: start all %d sensors ===\n", n);
  void *sensors[8] = {0};
  uint32_t w = 0, h = 0;
  q.query_dims(8, 0, &w, &h);
  if (!w || !h) { fprintf(stderr, "[-] no dims\n"); return -1; }
  uint32_t dim[2] = { w, h };
  unsigned char cfg[28] = {0};
  ((uint32_t *)cfg)[0] = 112;                 // mono8 cam_format

  int started = 0;
  for (int i = 0; i < n && i < 8; i++) {
    sensors[i] = q.get_sensor(hal, i);
    if (!sensors[i]) { printf("[-] cam %d: acquire failed\n", i); continue; }
    int rc = q.start(sensors[i], dim, cfg, 4, NULL, 1);
    int on = *(uint32_t *)((unsigned char *)sensors[i] + 0x0c);
    printf("[%c] cam %d: start_sensor rc=%d started=%u fd=%d\n",
           rc == 0 ? '+' : '-', i, rc, on, on ? q.get_fd(sensors[i]) : -1);
    if (on) started++;
  }
  printf("[*] %d/%d sensors streaming\n", started, n);
  if (!started) return -1;

  // MCU strobe must start after the v4l2 pipelines are up.
  printf("\n=== STAGE 4b: syncboss startCameras ===\n");
  syncboss_lib_start_streaming(n);
  usleep(400000);

  mkdir(OUTDIR, 0755);
  for (int r = 0; r < rounds; r++) {
    for (int i = 0; i < n; i++) {
      if (!sensors[i]) continue;
      void *frame = NULL;
      for (int t = 0; t < 40 && !frame; t++) { frame = q.dequeue(sensors[i], 0); if (!frame) usleep(5000); }
      if (!frame) { printf("[cam %d round %d] no frame\n", i, r); continue; }
      unsigned char *c = (unsigned char *)frame - 8;
      void *drvbuf = *(void **)c;
      void *px = *(void **)(c + 0x38);
      uint64_t ts_s = *(uint64_t *)(c + 0x08), ts_ns = *(uint64_t *)(c + 0x10);
      uint32_t sz = drvbuf ? *(uint32_t *)((unsigned char *)drvbuf + 0x2c) : 0;
      if (px && sz && sz <= 4u * 1024 * 1024) {
        unsigned char *buf = malloc(sz);
        if (buf && safe_peek(px, buf, sz) == 0) {
          unsigned long sum = 0; unsigned char mn = 255, mx = 0;
          for (uint32_t k = 0; k < sz; k++) { sum += buf[k]; if (buf[k] < mn) mn = buf[k]; if (buf[k] > mx) mx = buf[k]; }
          char path[160];
          snprintf(path, sizeof path, OUTDIR "/cam%d_%03d.gray", i, r);
          int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
          if (fd >= 0) { if (write(fd, buf, sz) < 0) {} close(fd); }
          printf("[cam %d round %d] ts=%llu.%09llu size=%u min=%u max=%u mean=%.1f -> %s\n",
                 i, r, (unsigned long long)ts_s, (unsigned long long)ts_ns, sz, mn, mx,
                 (double)sum / sz, path);
        }
        free(buf);
      }
      q.enqueue(sensors[i], frame);
    }
  }

  for (int i = 0; i < n; i++) {
    if (!sensors[i]) continue;
    if (*(uint32_t *)((unsigned char *)sensors[i] + 0x0c)) q.stop(sensors[i]);
    q.release(sensors[i]);
  }
  return 0;
}


// ── Stage 5: VIO capture ─────────────────────────────────────────────────────────────────────
// Streams a camera pair plus the syncboss IMU into a raw dataset for Basalt.
//
// Clocks: frames carry CLOCK_MONOTONIC (from the driver buffer). The syncboss IMU (type 0x50)
// is stamped on the nRF 1 MHz clock instead — but the same stream carries type 0x51 camera
// exposure packets at 30 Hz on that same nRF clock, so pairing 0x51 against our per-frame
// monotonic stamps yields the affine nRF->monotonic map (cf. notes/08). We therefore just dump
// the syncboss stream raw and let tools/vio/sb_decode.py do the decode host-side.
static volatile int g_imu_run = 1;

// Also log a CLOCK_MONOTONIC stamp against the byte offset of every read. Analysis of the first
// capture showed the 0xe0 exposure stamps and the 0x50 IMU stamps do NOT share an epoch (an ~816
// ms offset, found only by cross-correlating optical flow against gyro), so pairing them is not
// safe. With (host_time, byte_offset) pairs the nRF->monotonic map can be fitted directly and
// unambiguously, and every packet's host arrival time is bounded by its enclosing chunk.
static void *imu_thread(void *arg) {
  int out = *(int *)arg;
  unsigned char buf[65536];
  FILE *idx = fopen(OUTDIR "/syncboss_chunks.csv", "w");
  if (!idx) { fprintf(stderr, "[-] cannot open syncboss_chunks.csv: %m\n"); return NULL; }
  fprintf(idx, "#host_mono_ns,byte_offset,bytes\n");
  uint64_t off = 0;
  // poll() rather than a blocking read(), so the thread notices g_imu_run going false and can be
  // joined deterministically. A blocking read left the thread stuck at exit, which meant this
  // stdio buffer was never flushed and the tail of syncboss.raw could be lost.
  while (g_imu_run) {
    struct pollfd pfd = { .fd = sb_stream_fd, .events = POLLIN };
    int pr = poll(&pfd, 1, 100);
    if (pr <= 0) continue;
    ssize_t n = read(sb_stream_fd, buf, sizeof buf);
    if (n > 0) {
      uint64_t t = mono_ns();
      if (write(out, buf, n) != n) { fprintf(stderr, "[-] short write to syncboss.raw\n"); break; }
      fprintf(idx, "%llu,%llu,%zd\n", (unsigned long long)t, (unsigned long long)off, n);
      off += (uint64_t)n;
    } else if (n < 0 && errno != EAGAIN && errno != EINTR) {
      break;
    }
  }
  fflush(idx);
  fclose(idx);
  printf("[+] imu thread: %llu bytes of syncboss stream\n", (unsigned long long)off);
  return NULL;
}

// Stop (if started) and release every non-NULL sensor. Idempotent and NULL-safe so it can be
// used from any exit path.
static void release_sensors(void **sensors, int n) {
  for (int k = 0; k < n; k++) {
    if (!sensors[k]) continue;
    if (*(uint32_t *)((unsigned char *)sensors[k] + 0x0c)) q.stop(sensors[k]);
    q.release(sensors[k]);
    sensors[k] = NULL;
  }
}

static int stage_capture(void *hal, int camA, int camB, int seconds) {
  printf("\n=== STAGE 5: VIO capture (cams %d+%d, %d s) ===\n", camA, camB, seconds);
  int cams[2] = { camA, camB };
  void *sensors[2] = {0};
  uint32_t w = 0, h = 0;
  q.query_dims(8, 0, &w, &h);
  if (!w || !h) return -1;
  uint32_t dim[2] = { w, h };
  unsigned char cfg[28] = {0};
  ((uint32_t *)cfg)[0] = 112;

  // Single teardown path: anything acquired here is stopped and released on EVERY exit, or
  // qcamera_close later asserts "Cameras still in use!" and aborts, which would in turn skip
  // the syncboss teardown in main() and leave the cameras powered.
  for (int k = 0; k < 2; k++) {
    sensors[k] = q.get_sensor(hal, cams[k]);
    if (!sensors[k]) {
      printf("[-] cam %d acquire failed\n", cams[k]);
      release_sensors(sensors, 2);
      return -1;
    }
    int rc = q.start(sensors[k], dim, cfg, 8, NULL, 1);
    printf("[%c] cam %d start rc=%d started=%u\n", rc ? '-' : '+', cams[k], rc,
           *(uint32_t *)((unsigned char *)sensors[k] + 0x0c));
  }

  mkdir(OUTDIR, 0755);
  int raw = open(OUTDIR "/syncboss.raw", O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (raw < 0) {                             // without the IMU the capture is useless — bail now
    fprintf(stderr, "[-] cannot open syncboss.raw: %m\n");
    release_sensors(sensors, 2);
    return -1;
  }
  printf("[+] syncboss.raw fd=%d\n", raw);
  if (sb.imu_enable) printf("[*] syncboss_imu_enable rc=%d\n", sb.imu_enable(sb_handle));

  pthread_t th;
  int perr = pthread_create(&th, NULL, imu_thread, &raw);
  if (perr) {
    fprintf(stderr, "[-] pthread_create failed: %s\n", strerror(perr));
    close(raw);
    release_sensors(sensors, 2);
    return -1;
  }

  syncboss_lib_start_streaming(4);
  usleep(300000);

  FILE *idx = fopen(OUTDIR "/frames.csv", "w");
  fprintf(idx, "#seq,cam,ts_ns,file\n");
  uint64_t t_end = mono_ns() + (uint64_t)seconds * 1000000000ull;
  int seq = 0, wrote = 0;
  while (mono_ns() < t_end) {
    for (int k = 0; k < 2; k++) {
      void *frame = q.dequeue(sensors[k], 0);
      if (!frame) continue;
      unsigned char *c = (unsigned char *)frame - 8;
      void *drvbuf = *(void **)c;
      void *px = *(void **)(c + 0x38);
      uint64_t ts = *(uint64_t *)(c + 0x08) * 1000000000ull + *(uint64_t *)(c + 0x10);
      uint32_t sz = drvbuf ? *(uint32_t *)((unsigned char *)drvbuf + 0x2c) : 0;
      if (px && sz && sz <= 4u * 1024 * 1024) {
        char path[192];
        snprintf(path, sizeof path, OUTDIR "/c%d_%06d.gray", cams[k], seq);
        int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd >= 0) {
          if (write(fd, px, sz) == (ssize_t)sz) {
            fprintf(idx, "%d,%d,%llu,c%d_%06d.gray\n", seq, cams[k],
                    (unsigned long long)ts, cams[k], seq);
            wrote++;
          }
          close(fd);
        }
      }
      q.enqueue(sensors[k], frame);
    }
    seq++;
    usleep(2000);
  }
  fclose(idx);
  printf("[+] capture done: %d frames written over %d s\n", wrote, seconds);

  // Join the IMU thread BEFORE closing its fd, so syncboss.raw and syncboss_chunks.csv are
  // complete and flushed rather than truncated by process exit.
  g_imu_run = 0;
  pthread_join(th, NULL);
  close(raw);

  release_sensors(sensors, 2);
  return 0;
}

int main(int argc, char **argv) {
  const char *stage = argc > 1 ? argv[1] : "enum";
  int cam = argc > 2 ? atoi(argv[2]) : 0;
  int nframes = argc > 3 ? atoi(argv[3]) : 10;
  if (argc > 4) g_exposure = (uint16_t)atoi(argv[4]);
  if (argc > 5) g_gain = (uint16_t)atoi(argv[5]);

  setvbuf(stdout, NULL, _IOLBF, 0);            // line-buffered: keep output if we abort
  printf("cam_direct — B1 bring-up (stage=%s cam=%d)\n", stage, cam);

  if (load_vendor_stack() != 0) return 1;

  // Power the cameras before opening them, and hold the streaming fd for the whole run.
  if (!strcmp(stage, "start") || !strcmp(stage, "stream") || !strcmp(stage, "all") || !strcmp(stage, "capture")) {
    printf("\n=== STAGE 0b: syncboss camera power + FSIN strobe ===\n");
    syncboss_cam_power(1);            // raw probe: powers rails + MCLK (kernel-snooped)
    syncboss_lib_start(4, 30);        // libsyncboss: config + start_streaming => FSIN
    usleep(300000);                   // let the MCU come up before we open the pipeline
  }

  // Stage 0: dlopen + dlsym only. Answers the linker-namespace question (can a /data binary
  // load /system/vendor/lib64?) WITHOUT touching /dev — safe to run with the HAL still up.
  if (!strcmp(stage, "probe")) { printf("\n[+] stage 0 (probe) OK — no hardware touched\n"); return 0; }

  int n = 0, rc = 0;
  void *hal = stage_enum(&n);
  // Every exit from here on goes through `cleanup`: returning early would leave the cameras
  // powered and the FSIN strobe running.
  if (!hal) { rc = 1; goto cleanup; }
  if (cam >= n) {
    fprintf(stderr, "[-] cam %d >= num_sensors %d\n", cam, n);
    rc = 1;
    goto cleanup;
  }

  if (!strcmp(stage, "capture")) {
    // argv: capture <camA> <seconds> <exposure> <gain> ; camB defaults to camA+2 (the pair
    // validated by the earlier 0.378 m trajectory, exports/vio-precise/trajectory_calib02.txt)
    rc = stage_capture(hal, cam, cam + 2, nframes);
  } else if (!strcmp(stage, "all")) {
    rc = stage_all(hal, n, nframes);
  } else if (!strcmp(stage, "start") || !strcmp(stage, "stream")) {
    int variant = argc > 4 ? atoi(argv[4]) : 0;
    rc = stage_start(hal, cam, variant);
    if (rc == 0 && !strcmp(stage, "stream")) rc = stage_stream(hal, cam, nframes);
    // Cleanup must be NULL-guarded: a failed acquire previously turned into stop(NULL).
    void *s = g_sensor;
    if (s) {
      if (g_started) printf("\n[*] qcamera_stop_sensor rc=%d\n", q.stop(s));
      // Always release: qcamera_close asserts "Cameras still in use!" otherwise.
      printf("[*] qcamera_release_sensor rc=%d\n", q.release(s));
    }
  }

cleanup:
  if (hal) printf("[*] qcamera_close rc=%d\n", q.close(hal));
  syncboss_lib_stop();
  if (sb_fd >= 0) syncboss_cam_power(0);   // release the cameras and drop the streaming client
  printf("done.\n");
  return rc;
}
