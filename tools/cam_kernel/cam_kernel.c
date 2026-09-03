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
#include <poll.h>
#include <signal.h>
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


// ── ION buffers ──────────────────────────────────────────────────────────────────────────────
// The ISP writes through the SMMU, so frame buffers must be dmabufs, not plain malloc. Heap mask
// 0x02000000 is ION_SYSTEM_HEAP_ID=25, matching the vendor's allocations in the trace.
#define ION_HEAP_MASK (1u << 25)

struct ionbuf { int handle; int fd; size_t len; void *va; };

static int ion_fd = -1;

static int ion_alloc(struct ionbuf *b, size_t len) {
  struct ion_allocation_data a; memset(&a, 0, sizeof a);
  a.len = len; a.align = 4096; a.heap_id_mask = ION_HEAP_MASK; a.flags = 1;
  if (ioctl(ion_fd, ION_IOC_ALLOC, &a) < 0) { perror("ION_IOC_ALLOC"); return -1; }
  struct ion_fd_data f; memset(&f, 0, sizeof f);
  f.handle = a.handle;
  if (ioctl(ion_fd, ION_IOC_SHARE, &f) < 0) { perror("ION_IOC_SHARE"); return -1; }
  b->handle = a.handle; b->fd = f.fd; b->len = len;
  b->va = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_SHARED, f.fd, 0);
  if (b->va == MAP_FAILED) { perror("mmap ion"); b->va = NULL; return -1; }
  return 0;
}

// ── per-camera bring-up ──────────────────────────────────────────────────────────────────────
// Order and payloads replay the vendor sequence decoded in notes/19. Per-camera CSI values differ
// only in csid_core/phy_sel, except camera 3 which shares CSIPHY 2 with camera 2 in combo mode.
struct cam {
  int vfd_session;         // first open: creates the msm session, holds stream_id 0
  int vfd;                 // second open: the streaming handle, stream_id 1
  int phyfd, csidfd;       // subdev fds
  int vfefd;               // ISP subdev (cams 0,1 -> vfe0; 2,3 -> vfe1)
  uint32_t session, stream;
  uint32_t isp_handle;     // returned by ISP_REQUEST_BUF
  uint32_t axi_handle;     // returned by ISP_REQUEST_STREAM
  struct ionbuf buf[NBUF];
};

static const struct { unsigned char lane_mask_lo, lane_mask_hi, combo, core; unsigned short assign; }
  CSI_CFG[MAXCAM] = {
    { 0x03, 0x00, 0, 0, 0x4320 },
    { 0x03, 0x00, 0, 1, 0x4320 },
    { 0x03, 0x00, 0, 2, 0x4320 },
    { 0x18, 0x00, 1, 3, 0x0003 },   // shares CSIPHY 2 with camera 2, combo mode
  };
static const int PHY_SEL[MAXCAM] = { 0, 1, 2, 2 };

static int csid_version(int fd, uint32_t *ver) {
  struct csid_cfg_data c; memset(&c, 0, sizeof c);
  c.cfgtype = CSID_INIT;
  int r = xioctl(fd, VIDIOC_MSM_CSID_IO_CFG, &c, "CSID_INIT");
  if (r == 0) *ver = c.cfg.csid_version;
  return r;
}

// CSIPHY_INIT (cfgtype 0, all-zero union) powers and clocks the PHY. It must precede CSIPHY_CFG.
// Missing it was the last blocker: every other call still succeeded and the kernel logged nothing,
// but with an uninitialised PHY no CSI packets are ever received, so DQBUF simply never returns.
// Found by counting cfgtypes in the vendor trace -- it sends CSIPHY_INIT 8 times and CSIPHY_CFG 4,
// while cam_kernel sent CSIPHY_CFG only. (CSID_INIT we already sent, as the version query.)
static int csiphy_init(int fd) {
  struct csiphy_cfg_data c; memset(&c, 0, sizeof c);
  c.cfgtype = CSIPHY_INIT;
  return xioctl(fd, VIDIOC_MSM_CSIPHY_IO_CFG, &c, "CSIPHY_INIT");
}

// CSIPHY_RELEASE is NOT a bare command: msm_csiphy.c:1548 does copy_from_user() on
// cfg.csi_lane_params, so passing a zeroed union makes it fault with EFAULT and log
// "msm_csiphy_cmd: 1551 failed". It needs the same lane assign/mask the CFG uses.
static int csi_release(int phyfd, int csidfd, int i) {
  struct msm_camera_csi_lane_params lp;
  lp.csi_lane_assign = CSI_CFG[i].assign;
  lp.csi_lane_mask = (unsigned short)(CSI_CFG[i].lane_mask_lo | (CSI_CFG[i].lane_mask_hi << 8));
  struct csiphy_cfg_data p; memset(&p, 0, sizeof p);
  p.cfgtype = CSIPHY_RELEASE; p.cfg.csi_lane_params = &lp;
  int a = xioctl(phyfd, VIDIOC_MSM_CSIPHY_IO_CFG, &p, "CSIPHY_RELEASE");
  struct csid_cfg_data c; memset(&c, 0, sizeof c); c.cfgtype = CSID_RELEASE;
  int b = xioctl(csidfd, VIDIOC_MSM_CSID_IO_CFG, &c, "CSID_RELEASE");
  return (a < 0 || b < 0) ? -1 : 0;
}

static int csiphy_cfg(int fd, int i) {
  struct msm_camera_csiphy_params p; memset(&p, 0, sizeof p);
  p.lane_cnt = 1;
  p.settle_cnt = 14;
  p.lane_mask = (unsigned short)(CSI_CFG[i].lane_mask_lo | (CSI_CFG[i].lane_mask_hi << 8));
  p.combo_mode = CSI_CFG[i].combo;
  p.csid_core = CSI_CFG[i].core;
  struct csiphy_cfg_data c; memset(&c, 0, sizeof c);
  c.cfgtype = CSIPHY_CFG; c.cfg.csiphy_params = &p;
  return xioctl(fd, VIDIOC_MSM_CSIPHY_IO_CFG, &c, "CSIPHY_CFG");
}

static int csid_cfg(int fd, int i) {
  struct msm_camera_csid_vc_cfg vc; memset(&vc, 0, sizeof vc);
  vc.cid = 0; vc.dt = 0x2a; vc.decode_format = 1;   // 0x2a = RAW8 over CSI-2; see notes/19 gap
  struct msm_camera_csid_vc_cfg *vcp = &vc;
  struct msm_camera_csid_params p; memset(&p, 0, sizeof p);
  p.lane_cnt = 1;
  p.lane_assign = CSI_CFG[i].assign;
  p.phy_sel = (unsigned char)PHY_SEL[i];
  p.lut_params.num_cid = 1;
  p.lut_params.vc_cfg[0] = vcp;
  struct csid_cfg_data c; memset(&c, 0, sizeof c);
  c.cfgtype = CSID_CFG; c.cfg.csid_params = &p;
  return xioctl(fd, VIDIOC_MSM_CSID_IO_CFG, &c, "CSID_CFG");
}

static int ispif_call(int fd, int cfgtype, int vfe_intf, int csid, const char *what) {
  struct ispif_cfg_data c; memset(&c, 0, sizeof c);
  c.cfg_type = (enum ispif_cfg_type_t)cfgtype;
  c.params.num = 1;
  c.params.entries[0].vfe_intf = (enum msm_ispif_vfe_intf)vfe_intf;
  c.params.entries[0].intftype = RDI0;
  c.params.entries[0].num_cids = 1;
  c.params.entries[0].cids[0] = (enum msm_ispif_cid)0;
  c.params.entries[0].csid = (enum msm_ispif_csid)csid;
  c.params.entries[0].crop_enable = 0;
  return xioctl(fd, VIDIOC_MSM_ISPIF_CFG, &c, what);
}

// The vendor's per-camera ISPIF sequence is CFG -> CFG2 -> START_FRAME_BOUNDARY, not CFG -> START.
// CFG2 goes through VIDIOC_MSM_ISPIF_CFG_EXT, whose payload is { cfg_type, void *data, u32 size }
// pointing at a 724-byte msm_ispif_param_data_ext. Captured contents: the same single entry as
// CFG, with pack_cfg[] all zeros and stereo disabled -- so it adds no new values, but the call
// itself may still be required to arm the interface.
static int ispif_cfg2(int fd, int vfe_intf, int csid) {
  struct msm_ispif_param_data_ext e; memset(&e, 0, sizeof e);
  e.num = 1;
  e.entries[0].vfe_intf = (enum msm_ispif_vfe_intf)vfe_intf;
  e.entries[0].intftype = RDI0;
  e.entries[0].num_cids = 1;
  e.entries[0].cids[0] = (enum msm_ispif_cid)0;
  e.entries[0].csid = (enum msm_ispif_csid)csid;
  e.entries[0].crop_enable = 0;
  e.stereo_enable = 0;
  struct ispif_cfg_data_ext c; memset(&c, 0, sizeof c);
  c.cfg_type = ISPIF_CFG2;
  c.data = &e;
  c.size = sizeof e;
  return xioctl(fd, VIDIOC_MSM_ISPIF_CFG_EXT, &c, "ISPIF_CFG2");
}

// Initialise EVERY CSIPHY and CSID before any per-camera configuration, which is what the vendor
// does: a full INIT sweep across all subdevs up front, then per-camera CFG later. Doing INIT lazily
// per camera leaves the other PHYs unpowered, and on this rig cameras share PHYs (camera 3 runs in
// combo mode on CSIPHY 2), so a lazily-initialised PHY can be configured before it is ready.
// Fds are opened ONCE and held for the whole session, not opened per call.
//
// The first version of this sweep opened each subdev, sent INIT, and closed it. That undoes the
// init immediately: the CSID driver releases on last close (dmesg shows msm_csid_release), so the
// version query came back 0x00000000 where the vendor sees a non-zero version, and the hardware was
// torn down again before any camera was configured.
static int g_phyfd[MAXCAM], g_csidfd[MAXCAM];

static int csi_init_all(struct subdevs *sd) {
  for (int i = 0; i < sd->n_csiphy; i++) {
    g_phyfd[i] = open(sd->csiphy[i], O_RDWR);
    if (g_phyfd[i] < 0) { perror("open csiphy"); return -1; }
    csiphy_init(g_phyfd[i]);
  }
  for (int i = 0; i < sd->n_csid; i++) {
    g_csidfd[i] = open(sd->csid[i], O_RDWR);
    if (g_csidfd[i] < 0) { perror("open csid"); return -1; }
    uint32_t ver = 0;
    csid_version(g_csidfd[i], &ver);
    LOGV("    csid%d version=0x%08x\n", i, ver);
  }
  LOGV("[+] INIT sweep: %d csiphy, %d csid (fds held open)\n", sd->n_csiphy, sd->n_csid);
  return 0;
}

static int bringup_camera(struct cam *c, int i, struct subdevs *sd, int ispif_fd) {
  char *vp = sd->video[i];
  // O_NONBLOCK: a blocking DQBUF with no frames arriving hangs the process forever, and
  // because these devices are single-open that also blocks trackingservice from restarting.
  c->vfd = open(vp, O_RDWR | O_NONBLOCK);
  if (c->vfd < 0) { fprintf(stderr, "[-] open %s: %s\n", vp, strerror(errno)); return -1; }

  // session_id is the video node number (camera.c: pvdev->vdev->num), read via a private G_CTRL.
  struct v4l2_control ctl = { .id = MSM_CAMERA_PRIV_G_SESSION_ID, .value = 0 };
  if (xioctl(c->vfd, VIDIOC_G_CTRL, &ctl, "G_SESSION_ID") < 0) return -1;
  c->session = (uint32_t)ctl.value;

  // The video node must be opened TWICE, and stream_id 1 comes from the second handle.
  //
  // camera_v4l2_open() runs fh_open() BEFORE marking the node opened, and fh_open does
  //     stream_id = find_first_zero_bit(&atomic_read(&pvdev->opened))
  // so the FIRST handle gets stream_id 0 and also creates the msm session; the SECOND gets
  // stream_id 1. ISP_REQUEST_BUF rejects stream_id 0 outright (EINVAL), which is the tell.
  //
  // Getting this wrong is near-silent: with a mismatched stream id every ioctl still returns 0 --
  // the bufq is created and ADD_BUFQ binds it -- but msm_isp_get_buf() finds no vb2 queue for
  // (session, stream) and the ISP quietly writes every frame to a scratch buffer
  // ("msm_isp_get_stream_buffer() returned null"). That cost two debug cycles.
  // NOTE: the second open happens further down, in vendor order (after the CSI INIT/RELEASE
  // dance). An earlier restructure left a duplicate open here, so the node was opened THREE
  // times and the streaming handle landed on stream_id 2 instead of 1 -- silently, because
  // nothing errors: the ISP bufq is then created for stream 1 while the vb2 queue is registered
  // under stream 2, and frames go to a scratch buffer forever.

  // Opened once here and held for the whole session. Note these must NOT be closed between the
  // INIT/RELEASE/INIT dance below -- the CSID driver releases on last close, so closing would undo
  // the init we just performed.
  c->csidfd = (g_csidfd[i] > 0) ? g_csidfd[i] : (g_csidfd[i] = open(sd->csid[i], O_RDWR));
  c->phyfd  = (g_phyfd[PHY_SEL[i]] > 0) ? g_phyfd[PHY_SEL[i]]
                                        : (g_phyfd[PHY_SEL[i]] = open(sd->csiphy[PHY_SEL[i]], O_RDWR));
  c->vfefd  = open(sd->vfe[i / 2], O_RDWR);
  if (c->csidfd < 0 || c->phyfd < 0 || c->vfefd < 0) { fprintf(stderr, "[-] subdev fd\n"); return -1; }

  LOGV("[+] cam%d session=%u  (%s, %s, %s, vfe%d)\n",
       i, c->session, vp, sd->csid[i], sd->csiphy[PHY_SEL[i]], i / 2);

  // EXACT replay of the vendor bring-up order (notes/19). Two parts of it are counter-intuitive
  // and were both wrong in earlier attempts:
  //
  //   * CSID_INIT + CSIPHY_INIT are done on the FIRST video-node handle, then RELEASED, then done
  //     AGAIN on the second handle. Skipping the release/re-init leaves the CSI blocks associated
  //     with the wrong handle.
  //   * ISPIF_SET_VFE_INFO and ISPIF_INIT come AFTER STREAMON, not at startup. Doing them globally
  //     before any camera -- which is what "initialise the hardware first" intuition suggests --
  //     puts the ISPIF in the wrong state by the time the stream is configured.
  //
  // Everything returns 0 either way, so none of this shows up as an error; it only shows up as no
  // frames. The order came from a positional diff of our trace against the vendor's.
  // GET_SUBDEV_ID on CSIPHY and CSID. The vendor issues these before configuring, and a
  // driver-level diff of a working B1 run against ours showed msm_csid_get_subdev_id present in
  // B1 and absent here -- the only *causal* difference; every other B1-only code path was just a
  // downstream consequence of an interrupt that never fired for us. An earlier restructure dropped
  // these calls and left only a comment about them.
  { uint32_t sd_id = 0;
    xioctl(c->phyfd,  VIDIOC_MSM_SENSOR_GET_SUBDEV_ID, &sd_id, "CSIPHY_GET_SUBDEV_ID");
    sd_id = 0;
    xioctl(c->csidfd, VIDIOC_MSM_SENSOR_GET_SUBDEV_ID, &sd_id, "CSID_GET_SUBDEV_ID"); }

  uint32_t ver = 0;
  csid_version(c->csidfd, &ver);            // CSID_INIT
  csiphy_init(c->phyfd);
  usleep(2000);
  csi_release(c->phyfd, c->csidfd, i);      // CSIPHY_RELEASE + CSID_RELEASE
  usleep(2000);

  c->vfd_session = c->vfd;                  // first handle keeps the session
  c->vfd = open(vp, O_RDWR | O_NONBLOCK);   // second handle: stream_id 1
  if (c->vfd < 0) { fprintf(stderr, "[-] second open %s: %s\n", vp, strerror(errno)); return -1; }
  { struct v4l2_control c2 = { .id = MSM_CAMERA_PRIV_G_SESSION_ID, .value = 0 };
    xioctl(c->vfd, VIDIOC_G_CTRL, &c2, "G_SESSION_ID(2)"); }

  ver = 0;
  csid_version(c->csidfd, &ver);            // CSID_INIT again, on the streaming handle
  csiphy_init(c->phyfd);                    // CSIPHY_INIT again
  usleep(2000);
  LOGV("    csid_version=0x%08x\n", ver);

  for (int b = 0; b < NBUF; b++)
    if (ion_alloc(&c->buf[b], (FRAME_SZ + 4095) & ~4095u) < 0) return -1;

  // S_PARM registers the vb2 queue under (session, fh stream_id) AND writes that stream_id back
  // in parm.capture.extendedmode (camera.c: "use stream_id as stream index"). Read it rather than
  // assuming 1: the ISP bufq must be created for exactly this id or msm_isp_get_buf() finds no
  // vb2 queue and every frame silently lands in a scratch buffer.
  struct v4l2_streamparm parm; memset(&parm, 0, sizeof parm);
  parm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
  if (xioctl(c->vfd, VIDIOC_S_PARM, &parm, "S_PARM") < 0) return -1;
  c->stream = parm.parm.capture.extendedmode;
  LOGV("    fh stream_id (from S_PARM) = %u\n", c->stream);

  struct v4l2_format fmt; memset(&fmt, 0, sizeof fmt);
  fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
  struct msm_v4l2_format_data fd_; memset(&fd_, 0, sizeof fd_);
  fd_.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
  fd_.width = W; fd_.height = H;
  fd_.pixelformat = V4L2_PIX_FMT_GREY;
  fd_.num_planes = 1;
  fd_.plane_sizes[0] = FRAME_SZ;
  memcpy(fmt.fmt.raw_data, &fd_, sizeof fd_);
  if (xioctl(c->vfd, VIDIOC_S_FMT, &fmt, "S_FMT") < 0) return -1;

  struct v4l2_requestbuffers rb; memset(&rb, 0, sizeof rb);
  rb.count = NBUF; rb.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE; rb.memory = V4L2_MEMORY_USERPTR;
  if (xioctl(c->vfd, VIDIOC_REQBUFS, &rb, "REQBUFS") < 0) return -1;

  for (int b = 0; b < NBUF; b++) {
    // m.userptr carries the ION dmabuf FD, NOT a virtual address.
    //
    // msm abuses V4L2_MEMORY_USERPTR: msm_isp_copy_planes_from_v4l2_buffer() does
    //   qbuf_buf->planes[i].addr = vb2_buf->planes[i].m.userptr;
    // and msm_isp_prepare_v4l2_buf() then does
    //   mapped_info->buf_fd = qbuf_buf->planes[i].addr;   -> cam_smmu_get_phy_addr(fd)
    // so the field is consumed as a dmabuf fd. That is also why msm_vb2_dma_contig_get_userptr()
    // is a bare kzalloc+store stub -- there is no user pointer to map.
    //
    // Passing the real mmap'd address made the SMMU lookup fail and the ISP programmed a null DMA
    // target, which the instrumented kernel showed as:
    //   msm_isp_prepare_v4l2_buf: plane: 0 addr:0000000000000000
    // The evidence was in the vendor trace all along -- its plane had m.userptr = 0x21 (33), far
    // too small to be an address.
    struct v4l2_plane pl[1]; memset(pl, 0, sizeof pl);
    pl[0].m.userptr = (unsigned long)c->buf[b].fd;
    pl[0].length = (unsigned int)c->buf[b].len;
    struct v4l2_buffer vb; memset(&vb, 0, sizeof vb);
    vb.index = b; vb.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE; vb.memory = V4L2_MEMORY_USERPTR;
    vb.m.planes = pl; vb.length = 1;
    if (xioctl(c->vfd, VIDIOC_QBUF, &vb, "QBUF") < 0) return -1;
  }

  int type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
  if (xioctl(c->vfd, VIDIOC_STREAMON, &type, "STREAMON") < 0) return -1;

  // ISPIF globals AFTER streamon, per the vendor order.
  { struct ispif_cfg_data ic; memset(&ic, 0, sizeof ic);
    ic.cfg_type = ISPIF_SET_VFE_INFO; ic.vfe_info.num_vfe = sd->n_vfe;
    xioctl(ispif_fd, VIDIOC_MSM_ISPIF_CFG, &ic, "ISPIF_SET_VFE_INFO");
    memset(&ic, 0, sizeof ic);
    ic.cfg_type = ISPIF_INIT; ic.csid_version = 0x50000000;
    xioctl(ispif_fd, VIDIOC_MSM_ISPIF_CFG, &ic, "ISPIF_INIT"); }

  { struct msm_isp_ahb_clk_cfg ahb; memset(&ahb, 0, sizeof ahb); ahb.vote = 2;
    xioctl(c->vfefd, VIDIOC_MSM_ISP_AHB_CLK_CFG, &ahb, "ISP_AHB_CLK_CFG"); }

  if (csiphy_cfg(c->phyfd, i) < 0) return -1;
  usleep(2000);                             // let the PHY lock before CSID accepts the lanes
  if (csid_cfg(c->csidfd, i) < 0) return -1;
  usleep(2000);

  // The vendor sends security_mode=1 here, not 0. IOMMU_ATTACH is 0 in the enum, so
  // iommu_attach_mode=0 was already correct -- it was security_mode that was wrong.
  struct msm_vfe_smmu_attach_cmd sm; memset(&sm, 0, sizeof sm);
  sm.security_mode = 1; sm.iommu_attach_mode = IOMMU_ATTACH;
  xioctl(c->vfefd, VIDIOC_MSM_ISP_SMMU_ATTACH, &sm, "SMMU_ATTACH");

  // Each VFE has multiple RDI interfaces (VFE_RAW_0/1/2) and two cameras share a VFE here
  // (cam0,1 -> vfe0; cam2,3 -> vfe1), so both cameras on a VFE cannot claim VFE_RAW_0 -- the
  // second ISP_INPUT_CFG call fails with EINVAL, which is exactly what running with ncam=4
  // surfaced (cam1 and cam3 both failed). The vendor trace confirms byte-for-byte: input_src=1
  // (VFE_RAW_0) for the first camera on a VFE, input_src=2 (VFE_RAW_1) for the second; rdi_cfg.cid
  // stays 0 for both (cid is a CSID-side id, not per-VFE). This was invisible with ncam=1: a lone
  // camera on an otherwise-idle VFE always gets RAW_0 and the call always succeeds, which is
  // exactly the case every single-camera debug run before now exercised.
  struct msm_vfe_input_cfg icfg; memset(&icfg, 0, sizeof icfg);
  icfg.input_src = (enum msm_vfe_input_src)(VFE_RAW_0 + (i % 2));
  icfg.input_pix_clk = 48000000;
  icfg.d.rdi_cfg.cid = 0;
  icfg.d.rdi_cfg.frame_based = 1;
  if (xioctl(c->vfefd, VIDIOC_MSM_ISP_INPUT_CFG, &icfg, "ISP_INPUT_CFG") < 0) return -1;

  struct msm_vfe_axi_stream_request_cmd req; memset(&req, 0, sizeof req);
  req.session_id = c->session; req.stream_id = c->stream;
  req.output_format = V4L2_PIX_FMT_GREY;
  req.stream_src = (enum msm_vfe_axi_stream_src)(RDI_INTF_0 + (i % 2));
  req.frame_base = 1;
  if (xioctl(c->vfefd, VIDIOC_MSM_ISP_REQUEST_STREAM, &req, "ISP_REQUEST_STREAM") < 0) return -1;
  c->axi_handle = req.axi_stream_handle;

  // sub.type is an ISP event BITMASK here, not a v4l2 event type -- msm_isp_process_event_
  // subscription() walks it bit by bit against ISP_EVENT_MASK_INDEX_*. Passing V4L2_EVENT_ALL (0)
  // matches ISP_EVENT_SUBS_MASK_NONE, so it subscribed to NOTHING and returned success, which the
  // kernel logged as "Subs event_type is None=0x0" -- a message that was visible in dmesg for
  // several debug cycles before its meaning was chased down. 0x1dff is the mask the vendor uses
  // (STATS_NOTIFY, ERROR, IOMMU_P_FAULT, STREAM_UPDATE_DONE, REG_UPDATE, SOF, BUF_DIVERT, ...).
  struct v4l2_event_subscription sub; memset(&sub, 0, sizeof sub);
  sub.type = 0x1dff;
  xioctl(c->vfefd, VIDIOC_SUBSCRIBE_EVENT, &sub, "SUBSCRIBE_EVENT");

  struct msm_isp_buf_request br; memset(&br, 0, sizeof br);
  br.session_id = c->session; br.stream_id = c->stream;
  br.num_buf = NBUF; br.buf_type = ISP_PRIVATE_BUF;
  if (xioctl(c->vfefd, VIDIOC_MSM_ISP_REQUEST_BUF, &br, "ISP_REQUEST_BUF") < 0) return -1;
  c->isp_handle = br.handle;

  for (int b = 0; b < NBUF; b++) {
    struct msm_isp_qbuf_info qi; memset(&qi, 0, sizeof qi);
    qi.handle = c->isp_handle; qi.buf_idx = b;
    qi.buffer.num_planes = 1;
    qi.buffer.planes[0].addr = (uint32_t)c->buf[b].fd;
    qi.buffer.planes[0].offset = 0;
    qi.buffer.planes[0].length = (uint32_t)c->buf[b].len;
    if (xioctl(c->vfefd, VIDIOC_MSM_ISP_ENQUEUE_BUF, &qi, "ISP_ENQUEUE_BUF") < 0) return -1;
  }

  struct msm_vfe_axi_stream_update_cmd up; memset(&up, 0, sizeof up);
  up.num_streams = 1;
  up.update_type = UPDATE_STREAM_ADD_BUFQ;
  up.update_info[0].stream_handle = c->axi_handle;
  up.update_info[0].user_stream_id = c->stream;
  if (xioctl(c->vfefd, VIDIOC_MSM_ISP_UPDATE_STREAM, &up, "ISP_UPDATE_STREAM") < 0) return -1;

  struct msm_vfe_axi_stream_cfg_cmd cfg; memset(&cfg, 0, sizeof cfg);
  cfg.num_streams = 1;
  cfg.stream_handle[0] = c->axi_handle;
  cfg.cmd = START_STREAM;
  if (xioctl(c->vfefd, VIDIOC_MSM_ISP_CFG_STREAM, &cfg, "ISP_CFG_STREAM") < 0) return -1;

  // ── ISPIF ──
  int vfe_intf = i / 2;
  if (ispif_call(ispif_fd, ISPIF_CFG, vfe_intf, i, "ISPIF_CFG") < 0) return -1;
  if (ispif_cfg2(ispif_fd, vfe_intf, i) < 0) return -1;
  usleep(5000);
  if (ispif_call(ispif_fd, ISPIF_START_FRAME_BOUNDARY, vfe_intf, i, "ISPIF_START") < 0) return -1;
  LOGV("[+] cam%d pipeline up\n", i);
  return 0;
}

int main(int argc, char **argv) {
  int ncam = argc > 1 ? atoi(argv[1]) : 1;
  int secs = argc > 2 ? atoi(argv[2]) : 3;
  uint16_t exposure = argc > 3 ? (uint16_t)atoi(argv[3]) : 3000;
  uint16_t gain = argc > 4 ? (uint16_t)atoi(argv[4]) : 160;
  if (ncam > MAXCAM) ncam = MAXCAM;

  // Line-buffer stdout: this runs detached with output redirected to a file, so the default block
  // buffering means a hang produces an EMPTY log and tells you nothing about where it stopped.
  setvbuf(stdout, NULL, _IOLBF, 0);
  setvbuf(stderr, NULL, _IOLBF, 0);

  // Self-timeout. A hang here is worse than a crash: this process holds /dev/video* and
  // /dev/syncboss0, which are single-open, so trackingservice and the sensors HAL cannot restart
  // and sit in a "restarting" loop until it is killed. The restore watchdog cannot fix that -- it
  // restarts services, it does not kill us. So bound our own lifetime.
  signal(SIGALRM, SIG_DFL);
  alarm((unsigned)(secs + 30));
  printf("=== cam_kernel: B2, no Meta blobs (%d cam, %ds, exp=%u gain=%u) ===\n",
         ncam, secs, exposure, gain);

  struct subdevs sd;
  if (discover(&sd) < 0) { fprintf(stderr, "[-] media graph discovery failed\n"); return 1; }

  // Tell the kernel there is no camera daemon, so camera_v4l2_s_parm does not wait for one.
  // Hold msm_sensor_init open for the session. notes/11 attributes the in-kernel OV7251 CCI/I2C
  // bring-up to "the msm_sensor_init subdev probe", and neither trace shows ioctls on it -- so if
  // it matters at all, it matters at open() time. Cheap to hold; harmless if irrelevant.
  int sinitfd = -1;
  { char nm[64];
    for (int k = 0; k < 32; k++) {
      char node[32]; snprintf(node, sizeof node, "v4l-subdev%d", k);
      if (sysfs_name(node, nm, sizeof nm) == 0 && !strcmp(nm, "msm_sensor_init")) {
        char dev[40]; snprintf(dev, sizeof dev, "/dev/%s", node);
        sinitfd = open(dev, O_RDWR);
        LOGV("[%c] opened %s (msm_sensor_init) -> fd %d\n", sinitfd < 0 ? '-' : '+', dev, sinitfd);
        break;
      }
    } }

  int cfgfd = open("/dev/video0", O_RDWR);
  if (cfgfd >= 0) {
    struct msm_v4l2_event_data ed; memset(&ed, 0, sizeof ed);
    xioctl(cfgfd, MSM_CAM_V4L2_IOCTL_DAEMON_DISABLED, &ed, "DAEMON_DISABLED");
  }

  if (mcu_open_and_power() < 0) return 1;
  usleep(200000);
  mcu_configure(ncam, exposure, gain);

  ion_fd = open("/dev/ion", O_RDONLY);
  if (ion_fd < 0) { perror("open /dev/ion"); mcu_stop(); return 1; }

  int ispif_fd = open(sd.ispif, O_RDWR);
  if (ispif_fd < 0) { perror("open ispif"); mcu_stop(); return 1; }
  // ISPIF SET_VFE_INFO/INIT are issued per camera AFTER STREAMON -- see bringup_camera().

  static struct cam cams[MAXCAM];
  int up = 0;
  for (int i = 0; i < ncam; i++)
    if (bringup_camera(&cams[i], i, &sd, ispif_fd) == 0) up++;
  printf("[%c] %d/%d camera pipelines up\n", up == ncam ? '+' : '-', up, ncam);
  if (!up) { mcu_stop(); return 1; }

  // FSIN strobe LAST: the MCU must strobe into a receiver that is already listening (notes/13).
  mcu_start(ncam);
  usleep(400000);

  mkdir("/data/local/tmp/camkernel", 0755);
  int got[MAXCAM] = {0};
  double t_end = now_s() + secs;
  while (now_s() < t_end) {
    for (int i = 0; i < ncam; i++) {
      struct pollfd pfd = { .fd = cams[i].vfd, .events = POLLIN | POLLPRI };
      if (poll(&pfd, 1, 5) <= 0) continue;
      struct v4l2_plane dpl[1]; memset(dpl, 0, sizeof dpl);
      struct v4l2_buffer vb; memset(&vb, 0, sizeof vb);
      vb.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE; vb.memory = V4L2_MEMORY_USERPTR;
      vb.m.planes = dpl; vb.length = 1;
      if (ioctl(cams[i].vfd, VIDIOC_DQBUF, &vb) < 0) continue;
      if (got[i] < 3 && vb.index < NBUF) {
        const unsigned char *px = cams[i].buf[vb.index].va;
        unsigned long sum = 0;
        for (int k = 0; k < FRAME_SZ; k++) sum += px[k];
        char path[128];
        snprintf(path, sizeof path, "/data/local/tmp/camkernel/cam%d_%03d.gray", i, got[i]);
        int f = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (f >= 0) { if (write(f, px, FRAME_SZ) < 0) {} close(f); }
        printf("[cam %d frame %d] idx=%u ts=%ld.%06ld mean=%.1f -> %s\n", i, got[i], vb.index,
               (long)vb.timestamp.tv_sec, (long)vb.timestamp.tv_usec,
               (double)sum / FRAME_SZ, path);
      }
      got[i]++;
      dpl[0].m.userptr = (unsigned long)cams[i].buf[vb.index].fd;   // dmabuf fd, see QBUF note
      dpl[0].length = (unsigned int)cams[i].buf[vb.index].len;
      vb.m.planes = dpl; vb.length = 1;
      ioctl(cams[i].vfd, VIDIOC_QBUF, &vb);
    }
    usleep(2000);
  }
  // Decisive diagnostic: read the ION buffers DIRECTLY, regardless of DQBUF. This separates
  // "hardware never captured anything" from "hardware captured fine but the buffer-done handoff
  // back to userspace is broken" -- two completely different bugs that look identical from DQBUF.
  printf("\n[ION buffer contents, read directly - bypasses DQBUF entirely]\n");
  for (int i = 0; i < ncam; i++) {
    for (int b = 0; b < NBUF; b++) {
      const unsigned char *px = cams[i].buf[b].va;
      if (!px) continue;
      unsigned long sum = 0; unsigned nz = 0;
      for (int k = 0; k < FRAME_SZ; k++) { sum += px[k]; if (px[k]) nz++; }
      printf("  cam%d buf%d: mean=%.2f nonzero=%u/%d %s\n", i, b,
             (double)sum / FRAME_SZ, nz, FRAME_SZ,
             nz ? "<-- HAS DATA" : "(all zero)");
    }
  }

  printf("\n[frames dequeued] ");
  for (int i = 0; i < ncam; i++) printf("cam%d=%d ", i, got[i]);
  printf("\n");

  for (int i = 0; i < ncam; i++) {
    int t = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    ioctl(cams[i].vfd, VIDIOC_STREAMOFF, &t);
    ispif_call(ispif_fd, ISPIF_STOP_IMMEDIATELY, i / 2, i, "ISPIF_STOP");
  }
  close(ispif_fd);

  mcu_stop();
  if (cfgfd >= 0) close(cfgfd);
  return 0;
}
