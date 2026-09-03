// ioctl_trace.c — LD_PRELOAD shim recording every ioctl the vendor camera stack issues.
//
// Step 1.1 of notes/18. B2 reimplements libqcameradriver.so against the published kernel headers;
// this captures the reference trace to reimplement *against* — the exact ordering, the exact
// payloads, and which subdev each call lands on. Guessing that ordering from the headers alone is
// the difference between a week and a month.
//
// Why LD_PRELOAD rather than instrumenting cam_direct: the ioctls are issued from inside
// libqcameradriver.so, not from our code, so there is nothing of ours to instrument.
//
// Everything here goes through syscall() rather than dlsym(RTLD_NEXT) or stdio. The first attempt
// used both and segfaulted instantly: a constructor calling fopen() re-enters our own open()
// wrapper before dlsym has resolved the real one, and bionic routes open() through openat()
// anyway. Raw syscalls have no bootstrap ordering to get wrong and no re-entrancy.
//
// Records per ioctl: monotonic timestamp, tid, fd, the path the fd was opened with, request code
// and decoded name, _IOC_DIR/_IOC_SIZE, return value, and the payload as hex. Payloads are dumped
// generically from _IOC_SIZE rather than per-struct, so the trace is complete now and struct
// decoding can be done offline against the headers.
//
// Build: see build.sh.  Use: LD_PRELOAD=/data/local/tmp/libioctl_trace.so cam_direct ...
//        IOCTL_TRACE_OUT=/path/to/trace.log  (default /data/local/tmp/ioctl_trace.log)

#define _GNU_SOURCE
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <time.h>
#include <unistd.h>

#include <linux/ion.h>
#include <linux/media.h>
#include <linux/videodev2.h>
#include <media/msmb_camera.h>
#include <media/msmb_generic_buf_mgr.h>
#include <media/msmb_isp.h>
#include <media/msmb_ispif.h>
#include <media/msm_cam_sensor.h>

#include "msm_ioctl_table.h"

static int g_fd = -1;          // log fd, opened lazily on first use
static int g_opening;          // guards against re-entry while opening the log

static double now_s(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec + ts.tv_nsec * 1e-9;
}

static void logbuf(const char *buf, unsigned n) {
  if (g_fd < 0) {
    if (g_opening) return;                  // re-entered from our own openat wrapper
    g_opening = 1;
    const char *p = getenv("IOCTL_TRACE_OUT");
    if (!p) p = "/data/local/tmp/ioctl_trace.log";
    g_fd = (int)syscall(SYS_openat, AT_FDCWD, p, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    g_opening = 0;
    if (g_fd < 0) return;
    const char *h = "# t_mono tid fd path ioctl_name code dir size ret payload_hex\n";
    syscall(SYS_write, g_fd, h, (size_t)strlen(h));
  }
  syscall(SYS_write, g_fd, buf, (size_t)n);
}

static const char *name_for(unsigned long req) {
  for (size_t i = 0; i < sizeof msm_ioctls / sizeof msm_ioctls[0]; i++)
    if (msm_ioctls[i].code == req) return msm_ioctls[i].name;
  // Stock V4L2 codes we care about, spelled out because they are not in the MSM table.
  switch (req) {
    case VIDIOC_S_FMT:      return "VIDIOC_S_FMT";
    case VIDIOC_G_FMT:      return "VIDIOC_G_FMT";
    case VIDIOC_S_PARM:     return "VIDIOC_S_PARM";
    case VIDIOC_REQBUFS:    return "VIDIOC_REQBUFS";
    case VIDIOC_QUERYBUF:   return "VIDIOC_QUERYBUF";
    case VIDIOC_QBUF:       return "VIDIOC_QBUF";
    case VIDIOC_DQBUF:      return "VIDIOC_DQBUF";
    case VIDIOC_STREAMON:   return "VIDIOC_STREAMON";
    case VIDIOC_STREAMOFF:  return "VIDIOC_STREAMOFF";
    case VIDIOC_SUBSCRIBE_EVENT:   return "VIDIOC_SUBSCRIBE_EVENT";
    case VIDIOC_UNSUBSCRIBE_EVENT: return "VIDIOC_UNSUBSCRIBE_EVENT";
    case VIDIOC_DQEVENT:    return "VIDIOC_DQEVENT";
    case VIDIOC_QUERYCAP:   return "VIDIOC_QUERYCAP";
    case VIDIOC_G_CTRL:     return "VIDIOC_G_CTRL";
    case VIDIOC_S_CTRL:     return "VIDIOC_S_CTRL";
    // The vendor stack walks the media graph on /dev/media* to locate subdevs; these dominate the
    // trace by count (ENUM_ENTITIES ran 105 times in the reference capture).
    case MEDIA_IOC_DEVICE_INFO:   return "MEDIA_IOC_DEVICE_INFO";
    case MEDIA_IOC_ENUM_ENTITIES: return "MEDIA_IOC_ENUM_ENTITIES";
    case MEDIA_IOC_ENUM_LINKS:    return "MEDIA_IOC_ENUM_LINKS";
    case MEDIA_IOC_SETUP_LINK:    return "MEDIA_IOC_SETUP_LINK";
    case ION_IOC_ALLOC:     return "ION_IOC_ALLOC";
    case ION_IOC_FREE:      return "ION_IOC_FREE";
    case ION_IOC_SHARE:     return "ION_IOC_SHARE";
    case ION_IOC_IMPORT:    return "ION_IOC_IMPORT";
    case ION_IOC_SYNC:      return "ION_IOC_SYNC";
    case ION_IOC_CUSTOM:    return "ION_IOC_CUSTOM";
    default:                return NULL;   // caller prints type/nr so nothing stays opaque
  }
}

// Resolve fd -> path from /proc/self/fd on EVERY call.
//
// Two bugs led here. First version interposed open()/openat(); bionic inlines both to __openat
// under _FORTIFY_SOURCE, so the vendor lib never called the symbols we replaced and every fd came
// out "?". Second version read the link once and cached it -- but fds are closed and reused, so a
// reused number kept the old path and the trace showed ION_IOC_ALLOC arriving on
// /dev/v4l-subdev0, which is impossible. Re-reading each time is ~500 extra syscalls across a
// whole capture and removes the entire class of error. Do not "optimise" this back into a cache.
static const char *path_for(int fd, char *buf, size_t cap) {
  char lnk[64];
  snprintf(lnk, sizeof lnk, "/proc/self/fd/%d", fd);
  long n = syscall(SYS_readlinkat, AT_FDCWD, lnk, buf, cap - 1);
  if (n > 0) { buf[n] = 0; return buf; }
  return "?";
}

// Step 1.2 needs the syncboss MCU packet formats that libsyncboss.so sends (set_frame_rate,
// set_exposure_gain, start_streaming, probe/release). Rather than reverse the library, log the
// bytes it writes: the packets ARE the ABI. Only syncboss fds are logged, to keep the trace
// readable -- everything else in this process writes far more.
ssize_t write(int fd, const void *buf, size_t n) {
  ssize_t r = syscall(SYS_write, fd, buf, n);
  if (fd != g_fd) {
    char pb[64];
    const char *p = path_for(fd, pb, sizeof pb);
    if (strstr(p, "syncboss")) {
      char line[1024];
      int k = snprintf(line, sizeof line, "%.9f %d %d %s WRITE - 0 0 %d ",
                       now_s(), (int)syscall(SYS_gettid), fd, p, (int)r);
      unsigned m = n > 256 ? 256 : (unsigned)n;
      const unsigned char *q = buf;
      for (unsigned i = 0; i < m && k < (int)sizeof line - 4; i++)
        k += snprintf(line + k, sizeof line - k, "%02x", q[i]);
      k += snprintf(line + k, sizeof line - k, "\n");
      logbuf(line, (unsigned)k);
    }
  }
  return r;
}

// bionic declares `int ioctl(int, int, ...)` and marks it overloadable, so this must match that
// signature exactly or the compiler rejects the definition.
int ioctl(int fd, int op, ...) {
  va_list a;
  va_start(a, op);
  void *arg = va_arg(a, void *);
  va_end(a);

  unsigned long req = (unsigned int)op;
  double t0 = now_s();
  int ret = (int)syscall(SYS_ioctl, fd, req, arg);

  unsigned size = _IOC_SIZE(req);
  char pathbuf[64];
  const char *path = path_for(fd, pathbuf, sizeof pathbuf);

  const char *nm = name_for(req);
  char unk[32];
  if (!nm) {           // never leave a code opaque: type+nr is enough to find it in the headers
    snprintf(unk, sizeof unk, "?type=0x%02x,nr=%u",
             (unsigned)_IOC_TYPE(req), (unsigned)_IOC_NR(req));
    nm = unk;
  }
  static __thread char buf[4096];
  int n = snprintf(buf, sizeof buf, "%.9f %d %d %s %s 0x%lx %u %u %d ",
                   t0, (int)syscall(SYS_gettid), fd, path, nm, req,
                   (unsigned)_IOC_DIR(req), size, ret);
  // ISPIF_CFG_EXT is { cfg_type, void *data, u32 size } -- the real configuration (including
  // pack_cfg[], the CSI byte-packing setup) is entirely behind that pointer. Chase it too.
  if (req == VIDIOC_MSM_ISPIF_CFG_EXT && arg && size >= 20) {
    unsigned long dp; unsigned int dsz;
    memcpy(&dp, (const unsigned char *)arg + 8, 8);
    memcpy(&dsz, (const unsigned char *)arg + 16, 4);
    if (dsz > 1024) dsz = 1024;
    static unsigned char ext[1024];
    struct iovec l = { ext, dsz }, r = { (void *)dp, dsz };
    if (dp > 0x10000 && dsz &&
        syscall(SYS_process_vm_readv, syscall(SYS_getpid), &l, 1UL, &r, 1UL, 0UL) > 0) {
      n += snprintf(buf + n, sizeof buf - n, " extderef=");
      for (unsigned i = 0; i < dsz && n < (int)sizeof buf - 4; i++)
        n += snprintf(buf + n, sizeof buf - n, "%02x", ext[i]);
    }
  }
  // Dump the payload AFTER the call so _IOR/_IOWR results are captured too. Capped: a few of these
  // carry large embedded arrays and the interesting configuration is at the head.
  if (arg && size && size <= 4096) {
    unsigned k = size > 512 ? 512 : size;
    const unsigned char *p = arg;
    for (unsigned i = 0; i < k && n < (int)sizeof buf - 4; i++)
      n += snprintf(buf + n, sizeof buf - n, "%02x", p[i]);
    if (k < size) n += snprintf(buf + n, sizeof buf - n, "..+%u", size - k);
  } else {
    n += snprintf(buf + n, sizeof buf - n, "-");
  }
  // CSID/CSIPHY config is only 16 bytes on the wire -- { u32 cfgtype, union } -- and for the
  // *params cfgtypes the union is a pointer, so dumping the payload alone captures a pointer value
  // and none of the actual lane/clock/decode configuration.
  //
  // Follow it, but never by dereferencing directly: the same union also holds a plain u32 for the
  // version query (cfgtype 0 returns csid_version 0x50000000), and treating that as an address
  // segfaults the whole capture -- which is exactly what happened. process_vm_readv returns
  // -EFAULT instead of dying, so no cfgtype table is needed and an unexpected one cannot crash us.
  // CSID/CSIPHY config is only 16 bytes on the wire -- { u32 cfgtype, union } -- and for the
  // *_params cfgtypes the union is a pointer, so dumping the payload alone captures a pointer value
  // and none of the actual lane/clock/decode configuration.
  //
  // Follow it, but never by dereferencing directly: the same union also holds a plain u32 for the
  // version query (cfgtype 0 returns csid_version 0x50000000), and treating that as an address
  // segfaults the whole capture -- which is what happened on the first attempt. process_vm_readv
  // returns -EFAULT instead of dying, so no cfgtype table is needed and an unexpected one cannot
  // crash us.
  if ((req == VIDIOC_MSM_CSID_IO_CFG || req == VIDIOC_MSM_CSIPHY_IO_CFG) && arg && size >= 16) {
    unsigned long ptr; memcpy(&ptr, (const unsigned char *)arg + 8, 8);
    unsigned char tmp[256];
    struct iovec liov = { tmp, sizeof tmp }, riov = { (void *)ptr, sizeof tmp };
    long got = (ptr > 0x10000)
        ? syscall(SYS_process_vm_readv, syscall(SYS_getpid), &liov, 1UL, &riov, 1UL, 0UL) : -1;
    if (got > 0) {
      n += snprintf(buf + n, sizeof buf - n, " deref=");
      for (unsigned i = 0; i < sizeof tmp && n < (int)sizeof buf - 4; i++)
        n += snprintf(buf + n, sizeof buf - n, "%02x", tmp[i]);

      // SECOND-level chase, CSID only. msm_camera_csid_params has lut_params at offset 16 with an
      // INLINE vc_cfg_a[] at 17 and a POINTER array vc_cfg[] at 72. The vendor leaves vc_cfg_a
      // zeroed and fills the pointers, so the per-CID cid/dt/decode_format -- which decide whether
      // CSID accepts or silently drops every packet -- are one more indirection away and were
      // missing from the first capture entirely.
      if (req == VIDIOC_MSM_CSID_IO_CFG && got >= 80) {
        unsigned char num_cid = tmp[16];
        for (unsigned c = 0; c < num_cid && c < 4; c++) {
          unsigned long vp; memcpy(&vp, tmp + 72 + 8 * c, 8);
          unsigned char vc[3];
          struct iovec l2 = { vc, sizeof vc }, r2 = { (void *)vp, sizeof vc };
          if (vp > 0x10000 &&
              syscall(SYS_process_vm_readv, syscall(SYS_getpid), &l2, 1UL, &r2, 1UL, 0UL) > 0)
            n += snprintf(buf + n, sizeof buf - n, " vc_cfg[%u]=cid:%u,dt:0x%02x,dec:%u",
                          c, vc[0], vc[1], vc[2]);
        }
      }
    }
  }
  n += snprintf(buf + n, sizeof buf - n, "\n");
  logbuf(buf, (unsigned)n);
  return ret;
}
