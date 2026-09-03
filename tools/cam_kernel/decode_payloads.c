// decode_payloads.c — decode the ioctl payloads captured by ioctl_trace into named struct fields.
//
// Step 1.3 of notes/18. The trace gives complete payload hex; this turns it into the actual
// configuration values so the B2 reimplementation can be written with named parameters rather than
// opaque byte arrays replayed from a capture.
//
// It includes the SAME published kernel headers the device uses, so field offsets come from the
// compiler rather than from counting bytes by hand. Host and device are both LP64 with identical
// alignment rules for these plain-old-data structs, so decoding on the host is sound; anything
// with a pointer inside is flagged rather than trusted.
//
// Build: cc -I<kernel>/include/uapi -o decode_payloads decode_payloads.c
// Usage: decode_payloads <ioctl_trace.log>

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include <linux/videodev2.h>
#include <media/msmb_isp.h>
#include <media/msmb_ispif.h>
#include <media/msm_cam_sensor.h>

static int unhex(const char *h, unsigned char *out, int cap) {
  int n = 0;
  while (h[0] && h[1] && n < cap && h[0] != '.') {
    unsigned v; if (sscanf(h, "%2x", &v) != 1) break;
    out[n++] = (unsigned char)v; h += 2;
  }
  return n;
}

static const char *stream_src(int s) {
  switch (s) { case CAMIF: return "CAMIF"; case IDEAL_RAW: return "IDEAL_RAW";
    case RDI_INTF_0: return "RDI_INTF_0"; case RDI_INTF_1: return "RDI_INTF_1";
    case RDI_INTF_2: return "RDI_INTF_2"; case EXTERNAL_READ: return "EXTERNAL_READ";
    default: return "?"; }
}

static void do_request_stream(const unsigned char *b, int n) {
  if (n < (int)sizeof(struct msm_vfe_axi_stream_request_cmd)) {
    printf("    (truncated: %d of %zu bytes)\n", n, sizeof(struct msm_vfe_axi_stream_request_cmd));
  }
  struct msm_vfe_axi_stream_request_cmd c;
  memset(&c, 0, sizeof c); memcpy(&c, b, n < (int)sizeof c ? n : (int)sizeof c);
  printf("    session=%u stream=%u vt_enable=%u output_format=0x%08x (%.4s) stream_src=%s\n",
         c.session_id, c.stream_id, c.vt_enable, c.output_format,
         (const char *)&c.output_format, stream_src(c.stream_src));
  printf("    burst_count=%u hfr_mode=%u frame_base=%u init_frame_drop=%u skip_pattern=%d\n",
         c.burst_count, c.hfr_mode, c.frame_base, c.init_frame_drop, (int)c.frame_skip_pattern);
  printf("    buf_divert=%u controllable_output=%u burst_len=%u rdi_input_type=%d\n",
         c.buf_divert, c.controllable_output, c.burst_len, (int)c.rdi_input_type);
  for (int p = 0; p < 1; p++) {
    struct msm_vfe_axi_plane_cfg *pc = &c.plane_cfg[p];
    printf("    plane[%d]: out %ux%u stride=%u scanlines=%u plane_fmt=%u csid_src=%u rdi_cid=%u\n",
           p, pc->output_width, pc->output_height, pc->output_stride,
           pc->output_scan_lines, pc->output_plane_format, pc->csid_src, pc->rdi_cid);
  }
}

static void do_input_cfg(const unsigned char *b, int n) {
  struct msm_vfe_input_cfg c;
  memset(&c, 0, sizeof c); memcpy(&c, b, n < (int)sizeof c ? n : (int)sizeof c);
  static const char *isrc[] = {"VFE_PIX_0","VFE_RAW_0","VFE_RAW_1","VFE_RAW_2"};
  printf("    input_src=%d(%s) input_pix_clk=%u\n", (int)c.input_src,
         (int)c.input_src < 4 ? isrc[c.input_src] : "?", c.input_pix_clk);
  printf("    rdi_cfg: cid=%u frame_based=%u\n", c.d.rdi_cfg.cid, c.d.rdi_cfg.frame_based);
}

static const char *ispif_cfg_name(int t) {
  static const char *n[] = {"CLK_ENABLE","CLK_DISABLE","INIT","CFG","START_FRAME_BOUNDARY",
    "RESTART_FRAME_BOUNDARY","STOP_FRAME_BOUNDARY","STOP_IMMEDIATELY","RELEASE",
    "ENABLE_REG_DUMP","SET_VFE_INFO","CFG2","CFG_STEREO"};
  return (t >= 0 && t < (int)(sizeof n / sizeof *n)) ? n[t] : "?";
}
static const char *intf_name(int t) {
  static const char *n[] = {"PIX0","RDI0","PIX1","RDI1","RDI2"};
  return (t >= 0 && t < 5) ? n[t] : "?";
}

// The union member depends ENTIRELY on cfg_type. Reading it as `params` unconditionally made
// ISPIF_INIT report num=1342177280, which is really csid_version 0x50000000. Same class of mistake
// as dereferencing the csid version as a pointer -- decode the tag first, then the member.
static void do_ispif_cfg(const unsigned char *b, int n) {
  struct ispif_cfg_data c;
  memset(&c, 0, sizeof c); memcpy(&c, b, n < (int)sizeof c ? n : (int)sizeof c);
  printf("    cfg_type=%d (%s)\n", (int)c.cfg_type, ispif_cfg_name((int)c.cfg_type));
  if (c.cfg_type == ISPIF_INIT) { printf("    csid_version=0x%08x\n", c.csid_version); return; }
  if (c.cfg_type == ISPIF_SET_VFE_INFO) { printf("    vfe_info.num_vfe=%d\n", c.vfe_info.num_vfe); return; }
  if (c.cfg_type != ISPIF_CFG && c.cfg_type != ISPIF_CFG2 &&
      c.cfg_type != ISPIF_START_FRAME_BOUNDARY && c.cfg_type != ISPIF_STOP_FRAME_BOUNDARY &&
      c.cfg_type != ISPIF_STOP_IMMEDIATELY && c.cfg_type != ISPIF_RELEASE) return;
  printf("    num=%d\n", c.params.num);
  for (int i = 0; i < c.params.num && i < 4; i++) {
    struct msm_ispif_params_entry *e = &c.params.entries[i];
    printf("    entry[%d]: vfe_intf=%d intftype=%d(%s) num_cids=%d cids=[%d] csid=%d crop_en=%d\n",
           i, (int)e->vfe_intf, (int)e->intftype, intf_name((int)e->intftype), e->num_cids,
           (int)e->cids[0], (int)e->csid, e->crop_enable);
  }
}

// CSI params live behind the union pointer; ioctl_trace captures them as a trailing "deref=" field.
// Decoding these by eye went wrong twice (settle_cnt read as lane_mask), so they get the same
// compiler-checked treatment as everything else.
static void do_csiphy(const unsigned char *b, int n) {
  struct msm_camera_csiphy_params c; memset(&c, 0, sizeof c);
  memcpy(&c, b, n < (int)sizeof c ? n : (int)sizeof c);
  printf("    lane_cnt=%u settle_cnt=%u lane_mask=0x%04x combo_mode=%u csid_core=%u "
         "csiphy_clk=%u csi_3phase=%u data_rate=%llu\n",
         c.lane_cnt, c.settle_cnt, c.lane_mask, c.combo_mode, c.csid_core,
         c.csiphy_clk, c.csi_3phase, (unsigned long long)c.data_rate);
}
static void do_csid(const unsigned char *b, int n) {
  struct msm_camera_csid_params c; memset(&c, 0, sizeof c);
  memcpy(&c, b, n < (int)sizeof c ? n : (int)sizeof c);
  printf("    lane_cnt=%u lane_assign=0x%04x phy_sel=%u csi_clk=%u csi_3p_sel=%u num_cid=%u\n",
         c.lane_cnt, c.lane_assign, c.phy_sel, c.csi_clk, c.csi_3p_sel, c.lut_params.num_cid);
  // lut_params.vc_cfg is an array of POINTERS into the traced process, so the vc/dt entries are
  // not recoverable from this trace -- following them here segfaulted the decoder, the third time
  // a pointer inside one of these unions has bitten. ioctl_trace would need to chase them at
  // capture time. Reported as a known gap rather than silently printing garbage.
  if (c.lut_params.num_cid)
    printf("      (%u vc_cfg entries live behind pointers; not captured -- see notes/19)\n",
           c.lut_params.num_cid);
}

int main(int argc, char **argv) {
  if (argc < 2) { fprintf(stderr, "usage: %s <ioctl_trace.log>\n", argv[0]); return 1; }
  FILE *f = fopen(argv[1], "r");
  if (!f) { perror(argv[1]); return 1; }
  char line[8192];
  int seen_req = 0, seen_in = 0;
  int isp_seen[16] = {0};
  while (fgets(line, sizeof line, f)) {
    if (line[0] == '#') continue;
    char name[128] = "", hex[6144] = "";
    // fields: t tid fd path name code dir size ret payload
    if (sscanf(line, "%*s %*s %*s %*s %127s %*s %*s %*s %*s %6143s", name, hex) != 2) continue;
    unsigned char b[4096];
    int n = unhex(hex, b, sizeof b);
    if (n <= 0) continue;
    // deref= is appended after the payload for the CSI configs
    char *dr = strstr(line, "deref=");
    if (dr) {
      unsigned char db[256]; int dn = unhex(dr + 6, db, sizeof db);
      // GATE ON cfgtype. This is the fourth time a tagged union has produced confident nonsense:
      // only CSIPHY_CFG / CSID_CFG carry a params pointer, and decoding the INIT calls' unions as
      // params yielded lane_cnt=32, settle_cnt=67, data_rate=9369376207381987455 -- values obvious
      // enough to catch, which is luck, not method. Read the tag, then the member. Every time.
      unsigned int cfgtype = 0;
      { unsigned char pb[16]; if (unhex(hex, pb, sizeof pb) >= 4) memcpy(&cfgtype, pb, 4); }
      static int sp = 0, sc = 0;
      if (!strcmp(name, "VIDIOC_MSM_CSIPHY_IO_CFG") && cfgtype == CSIPHY_CFG && sp++ < 4) {
        printf("VIDIOC_MSM_CSIPHY_IO_CFG (cfgtype=CSIPHY_CFG) deref\n"); do_csiphy(db, dn);
      } else if (!strcmp(name, "VIDIOC_MSM_CSID_IO_CFG") && cfgtype == CSID_CFG && sc++ < 4) {
        printf("VIDIOC_MSM_CSID_IO_CFG (cfgtype=CSID_CFG) deref\n"); do_csid(db, dn);
      }
    }
    if (!strcmp(name, "VIDIOC_MSM_ISP_REQUEST_STREAM") && !seen_req++) {
      printf("VIDIOC_MSM_ISP_REQUEST_STREAM (%d B)\n", n); do_request_stream(b, n);
    } else if (!strcmp(name, "VIDIOC_MSM_ISP_INPUT_CFG") && !seen_in++) {
      printf("VIDIOC_MSM_ISP_INPUT_CFG (%d B)\n", n); do_input_cfg(b, n);
    } else if (!strcmp(name, "VIDIOC_MSM_ISPIF_CFG")) {
      struct ispif_cfg_data probe; memset(&probe,0,sizeof probe);
      memcpy(&probe, b, n < (int)sizeof probe ? n : (int)sizeof probe);
      int t = (int)probe.cfg_type;
      if (t < 0 || t > 15 || isp_seen[t]++) continue;
      printf("VIDIOC_MSM_ISPIF_CFG (%d B)\n", n); do_ispif_cfg(b, n);
    }
  }
  fclose(f);
  return 0;
}
