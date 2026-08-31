// img_shim.c — LD_PRELOAD into trackingservice. Camera frame tap, v2.
// A background thread (started from a library constructor) scans /proc/self/maps for
// anon_inode:dmabuf regions of camera-frame size and reads them DIRECTLY via their mapped VA
// (works in-process even for VM_PFNMAP). It dumps every frame-sized buffer that has real image
// content to /data/local/tmp/frame_<addr>.gray (latest snapshot), and logs per-buffer stats so
// we can see which buffers are active/changing while cameras stream (tracking/passthrough).

#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>
#include <fcntl.h>
#include <unistd.h>
#include <pthread.h>

#define LOG "/data/local/tmp/img_tap.log"
#define FRAME_MIN 300000
#define FRAME_MAX 400000

static void tlog(const char* fmt, ...) {
  char b[512]; va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof(b), fmt, ap); va_end(ap);
  int fd = open(LOG, O_WRONLY|O_CREAT|O_APPEND, 0666);
  if (fd >= 0) { if (write(fd, b, strlen(b))) {} close(fd); }
}

struct region { uintptr_t a, b; };

static int scan_maps(struct region* out, int max) {
  FILE* f = fopen("/proc/self/maps", "r");
  if (!f) return 0;
  char line[512]; int n = 0;
  while (fgets(line, sizeof(line), f) && n < max) {
    if (!strstr(line, "anon_inode:dmabuf")) continue;
    uintptr_t a, b;
    if (sscanf(line, "%lx-%lx", &a, &b) != 2) continue;
    size_t sz = b - a;
    if (sz < FRAME_MIN || sz > FRAME_MAX) continue;
    // must be readable
    if (strncmp(strchr(line,' ')+1, "r", 1) != 0) continue;
    out[n].a = a; out[n].b = b; n++;
  }
  fclose(f);
  return n;
}

static void* scanner(void* arg) {
  (void)arg;
  tlog("[scanner v2 started]\n");
  int cyc = 0;
  for (;;) {
    usleep(250000);
    cyc++;
    struct region rg[128];
    int n = scan_maps(rg, 128);
    if (cyc == 1 || cyc % 20 == 0) tlog("[cyc %d] dmabuf frame-regions=%d\n", cyc, n);
    for (int i = 0; i < n; i++) {
      const volatile uint8_t* p = (const volatile uint8_t*)rg[i].a;
      size_t sz = rg[i].b - rg[i].a, N = sz < 307200 ? sz : 307200;
      uint64_t sum = 0; uint32_t nz = 0, mx = 0, mn = 255;
      // direct CPU reads of the mapped VA (in-process; ok for PFNMAP)
      for (size_t k = 0; k < N; k += 7) { uint8_t v = p[k]; sum += v; if (v) nz++; if (v>mx) mx=v; if (v<mn) mn=v; }
      double mean = (double)sum / (N/7);
      // "real image": spread of values + not-all-zero + not-all-constant
      int looks_img = (mx > 40 && mn < mx - 30 && nz > (N/7)/10);
      if (looks_img) {
        if (cyc % 8 == 0) tlog("  frame @%lx sz=%zu mean=%.0f min=%u max=%u nz=%.0f%%\n",
                               rg[i].a, sz, mean, mn, mx, 100.0*nz/(N/7));
        char op[96]; snprintf(op, sizeof(op), "/data/local/tmp/frame_%lx.gray", rg[i].a);
        int of = open(op, O_WRONLY|O_CREAT|O_TRUNC, 0644);
        if (of >= 0) {
          // copy through a local buffer (volatile read) to avoid write() copy_from_user on PFNMAP
          static uint8_t buf[400000];
          for (size_t k = 0; k < sz && k < sizeof(buf); k++) buf[k] = p[k];
          if (write(of, buf, sz < sizeof(buf) ? sz : sizeof(buf))) {}
          close(of);
        }
      }
    }
  }
  return 0;
}

__attribute__((constructor))
static void img_shim_init(void) {
  pthread_t t;
  if (pthread_create(&t, 0, scanner, 0) == 0) pthread_detach(t);
}
