// cam_kernel.c — B2: drive the Quest tracking cameras with NO Meta userspace blobs.
//
// Step 1.3 of notes/18. Replaces libqcameraoculushal.so + libqcameradriver.so with direct ioctls on
// the published msm camera_v2 ABI, and libsyncboss.so with raw /dev/syncboss0 packets (step 1.2,
// already proven in cam_direct under SYNCBOSS_RAW=1). Together those are the last three closed Meta
// libraries in the camera path, and notes/16 established the OS swap deletes /vendor, so nothing
// built on them survives it.
//
// The configuration here is not guessed. notes/19 traced a working vendor session, decoded every
// payload against the published headers, and this replays that configuration with named
// parameters. The single most useful finding: the Quest uses the **RDI raw-dump path, not CAMIF**
// (stream_src=RDI_INTF_0, input_src=VFE_RAW_0, intftype=RDI0, frame_based=1), so none of the ISP
// pixel-processing configuration is involved -- CSI frames go straight to memory.
//
// Debugging strategy: run this under tools/cam_kernel/libioctl_trace.so and diff the resulting
// trace against the vendor reference trace. Any divergence in order or payload is the bug.
//
// Build: see build_cam_kernel.sh.   Run: cam_kernel [ncams] [seconds] [exposure] [gain]

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <linux/ion.h>
#include <linux/media.h>
#include <linux/videodev2.h>
#include <media/msmb_camera.h>
#include <media/msmb_isp.h>
#include <media/msmb_ispif.h>
#include <media/msm_cam_sensor.h>

#define MAXCAM 4
#define W 640
#define H 481
#define FRAME_SZ (W * H)
#define NBUF 4

static int g_verbose = 1;
#define LOGV(...) do { if (g_verbose) printf(__VA_ARGS__); } while (0)

static double now_s(void) {
  struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec + ts.tv_nsec * 1e-9;
}

static int xioctl(int fd, unsigned long req, void *arg, const char *what) {
  int r = ioctl(fd, (int)req, arg);
  if (r < 0) fprintf(stderr, "[-] %s: %s\n", what, strerror(errno));
  return r;
}

// ── syncboss MCU (step 1.2; formats from notes/19) ───────────────────────────────────────────
#define SB_DEV "/dev/syncboss0"
#define SB_STREAM "/dev/syncboss_stream0"
static int sb_fd = -1, sb_stream_fd = -1;
static unsigned char sb_seq = 0xa0;

static int sb_send(const char *what, unsigned char type, unsigned char seq,
                   const void *data, unsigned char len) {
  unsigned char pkt[64];
  pkt[0] = type; pkt[1] = seq; pkt[2] = len;
  if (len) memcpy(pkt + 3, data, len);
  ssize_t n = write(sb_fd, pkt, (size_t)len + 3);
  LOGV("[%c] mcu %-24s type=0x%02x -> %zd\n", n == (ssize_t)len + 3 ? '+' : '-', what, type, n);
  return n == (ssize_t)len + 3 ? 0 : -1;
}
static int sb_prop(const char *what, unsigned char prop, const void *val, unsigned char vlen) {
  unsigned char d[8]; d[0] = prop; memcpy(d + 1, val, vlen);
  int rc = sb_send(what, 0x03, sb_seq++, d, (unsigned char)(vlen + 1));
  usleep(20000);
  return rc;
}
static int mcu_open_and_power(void) {
  sb_stream_fd = open(SB_STREAM, O_RDONLY);   // held open: the driver releases cameras otherwise
  sb_fd = open(SB_DEV, O_RDWR);
  if (sb_fd < 0) { perror("open " SB_DEV); return -1; }
  unsigned char none = 0;
  return sb_send("camera_probe(power on)", 0x28, 0, &none, 0);
}
static int mcu_configure(int ncam, uint16_t exposure, uint16_t gain) {
  unsigned char n8 = (unsigned char)ncam, bpp = 8, tag = 1;
  uint32_t period = 33333;
  int rc = 0;
  rc |= sb_prop("set_bpp(8)", 0x8c, &bpp, 1);
  rc |= sb_send("camera_init", 0x2e, 0, &n8, 1);
  rc |= sb_prop("set_frame_rate(33333us)", 0x89, &period, 4);
  if (exposure) {
    uint16_t eg[8] = { exposure, exposure, exposure, exposure, gain, gain, gain, gain };
    rc |= sb_send("set_exposure_gain", 0x2a, 0, eg, sizeof eg);
  }
  rc |= sb_prop("set_frame_tag_mode(1)", 0x8d, &tag, 1);
  return rc;
}
static int mcu_start(int ncam) {
  unsigned char n8 = (unsigned char)ncam;
  return sb_send("start_streaming <-- FSIN", 0x2c, 0, &n8, 1);
}
static void mcu_stop(void) {
  unsigned char x = 0x98, none = 0;
  sb_send("stop_streaming", 0x2d, sb_seq++, &x, 1);
  sb_send("camera_deinit", 0x2f, sb_seq++, &x, 1);
  sb_send("camera_release(power off)", 0x29, 0, &none, 0);
  if (sb_fd >= 0) close(sb_fd);
  if (sb_stream_fd >= 0) close(sb_stream_fd);
}

// ── media graph discovery ────────────────────────────────────────────────────────────────────
// The vendor walks /dev/media* with ENUM_ENTITIES (101 calls in the reference trace) to find which
// /dev/v4l-subdevN is which block. Enumerating rather than hardcoding indices means the tool keeps
// working if the probe order changes, which is exactly the kind of thing that differs between
// kernel builds -- and we are heading for a new kernel.
struct subdevs {
  char csiphy[MAXCAM][32];
  char csid[MAXCAM][32];
  char ispif[32];
  char vfe[2][32];
  char video[MAXCAM][32];
  int n_csiphy, n_csid, n_vfe, n_video;
};

// Identify blocks by their sysfs `name` attribute, not by media-graph entity names.
//
// MEDIA_IOC_ENUM_ENTITIES was the obvious route (it is what the vendor does, 101 calls in the
// reference trace) but on this kernel the entity names are just the node names -- "v4l-subdev0",
// not "msm_csiphy0" -- so nothing can be classified from them. /sys/class/video4linux/<node>/name
// carries the real driver names:
//     v4l-subdev0,1,2   msm_csiphy      video0   msm-config
//     v4l-subdev3..6    msm_csid        video3-6 msm-sensor
//     v4l-subdev10,11   vfe
//     v4l-subdev12      msm_ispif
//     v4l-subdev13      msm_buf_mngr
// which matches the topology the trace showed exactly. The vendor gets there via
// VIDIOC_MSM_SENSOR_GET_SUBDEV_ID probes instead; sysfs is the same information, cheaper.
static int sysfs_name(const char *node, char *out, size_t cap) {
  char p[128]; snprintf(p, sizeof p, "/sys/class/video4linux/%s/name", node);
  FILE *f = fopen(p, "r"); if (!f) return -1;
  if (!fgets(out, (int)cap, f)) { fclose(f); return -1; }
  fclose(f);
  out[strcspn(out, "\n")] = 0;
  return 0;
}

static int discover(struct subdevs *s) {
  memset(s, 0, sizeof *s);
  char node[32], name[64];
  for (int i = 0; i < 32; i++) {
    snprintf(node, sizeof node, "v4l-subdev%d", i);
    if (sysfs_name(node, name, sizeof name) < 0) continue;
    char dev[32]; snprintf(dev, sizeof dev, "/dev/%s", node);
    if (!strcmp(name, "msm_csiphy") && s->n_csiphy < MAXCAM)
      snprintf(s->csiphy[s->n_csiphy++], 32, "%s", dev);
    else if (!strcmp(name, "msm_csid") && s->n_csid < MAXCAM)
      snprintf(s->csid[s->n_csid++], 32, "%s", dev);
    else if (!strcmp(name, "msm_ispif")) snprintf(s->ispif, 32, "%s", dev);
    else if (!strcmp(name, "vfe") && s->n_vfe < 2)
      snprintf(s->vfe[s->n_vfe++], 32, "%s", dev);
    LOGV("    %-16s %-16s -> %s\n", node, name, dev);
  }
  for (int i = 0; i < 40; i++) {
    snprintf(node, sizeof node, "video%d", i);
    if (sysfs_name(node, name, sizeof name) < 0) continue;
    if (!strcmp(name, "msm-sensor") && s->n_video < MAXCAM)
      snprintf(s->video[s->n_video++], 32, "/dev/%s", node);
    if (!strcmp(name, "msm-config")) LOGV("    %-16s %-16s -> /dev/%s (msm-config)\n", node, name, node);
  }
  LOGV("[+] discovered: %d csiphy, %d csid, %d vfe, ispif=%s, %d video\n",
       s->n_csiphy, s->n_csid, s->n_vfe, s->ispif[0] ? s->ispif : "?", s->n_video);
  return (s->n_csiphy && s->n_csid && s->n_vfe && s->ispif[0] && s->n_video) ? 0 : -1;
}

int main(int argc, char **argv) {
  int ncam = argc > 1 ? atoi(argv[1]) : 1;
  int secs = argc > 2 ? atoi(argv[2]) : 3;
  uint16_t exposure = argc > 3 ? (uint16_t)atoi(argv[3]) : 3000;
  uint16_t gain = argc > 4 ? (uint16_t)atoi(argv[4]) : 160;
  if (ncam > MAXCAM) ncam = MAXCAM;
  printf("=== cam_kernel: B2, no Meta blobs (%d cam, %ds, exp=%u gain=%u) ===\n",
         ncam, secs, exposure, gain);

  struct subdevs sd;
  if (discover(&sd) < 0) { fprintf(stderr, "[-] media graph discovery failed\n"); return 1; }

  // Tell the kernel there is no camera daemon, so camera_v4l2_s_parm does not wait for one.
  int cfgfd = open("/dev/video0", O_RDWR);
  if (cfgfd >= 0) {
    struct msm_v4l2_event_data ed; memset(&ed, 0, sizeof ed);
    xioctl(cfgfd, MSM_CAM_V4L2_IOCTL_DAEMON_DISABLED, &ed, "DAEMON_DISABLED");
  }

  if (mcu_open_and_power() < 0) return 1;
  usleep(200000);
  mcu_configure(ncam, exposure, gain);

  printf("\n[stage] this build stops after MCU configuration + graph discovery.\n");
  printf("        Next: per-camera CSIPHY/CSID/ISPIF/VFE bring-up (notes/19 sequence).\n");

  mcu_stop();
  if (cfgfd >= 0) close(cfgfd);
  return 0;
}
