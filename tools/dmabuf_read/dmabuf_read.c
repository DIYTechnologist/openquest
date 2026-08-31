// dmabuf_read.c — re-open another process's frame dmabuf fds via /proc/<pid>/fd, mmap them
// with DMA_BUF_IOCTL_SYNC, and read pixels. Proves camera frames are tappable from the shared
// ION buffers without driving V4L2 or beating the HAL.
//
// Usage: dmabuf_read <pid> [frame_size] [out_prefix]
//   iterates /proc/<pid>/fd/*, finds dmabufs, and for each of matching size dumps pixel stats
//   (min/max/mean/nonzero%) + a small hex sample; optionally writes the raw frame to a file.
//
// Build: NDK aarch64 clang -O1 -fPIE -pie -o dmabuf_read dmabuf_read.c

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <stdint.h>

struct dma_buf_sync { uint64_t flags; };
#define DMA_BUF_SYNC_READ  (1ull << 0)
#define DMA_BUF_SYNC_START (0ull << 2)
#define DMA_BUF_SYNC_END   (1ull << 2)
#define DMA_BUF_BASE 'b'
#define DMA_BUF_IOCTL_SYNC _IOW(DMA_BUF_BASE, 0, struct dma_buf_sync)

int main(int argc, char** argv) {
  if (argc < 2) { fprintf(stderr, "usage: %s <pid> [frame_size] [out_prefix]\n", argv[0]); return 2; }
  int pid = atoi(argv[1]);
  size_t want = argc > 2 ? (size_t)strtoul(argv[2], 0, 0) : 311296; // 640x486
  const char* outpfx = argc > 3 ? argv[3] : NULL;

  char dpath[64]; snprintf(dpath, sizeof(dpath), "/proc/%d/fd", pid);
  DIR* d = opendir(dpath);
  if (!d) { perror("opendir /proc/pid/fd"); return 1; }

  // histogram mode: want==0 -> just tally distinct dmabuf sizes
  size_t hs[64]; int hc[64]; int nh = 0;

  struct dirent* e;
  int matched = 0, dmabufs = 0;
  while ((e = readdir(d))) {
    if (e->d_name[0] < '0' || e->d_name[0] > '9') continue;
    char lp[80], tgt[256];
    snprintf(lp, sizeof(lp), "/proc/%d/fd/%s", pid, e->d_name);
    ssize_t n = readlink(lp, tgt, sizeof(tgt)-1);
    if (n <= 0) continue; tgt[n] = 0;
    if (!strstr(tgt, "dmabuf")) continue;
    dmabufs++;

    if (want == 0) {
      int fd = open(lp, O_RDONLY); if (fd < 0) continue;
      off_t sz = lseek(fd, 0, SEEK_END); close(fd);
      int f = 0; for (int i=0;i<nh;i++) if (hs[i]==(size_t)sz){hc[i]++;f=1;break;}
      if (!f && nh<64){ hs[nh]=(size_t)sz; hc[nh]=1; nh++; }
      continue;
    }

    int fd = open(lp, O_RDONLY);           // re-open the dmabuf via procfs magic symlink
    if (fd < 0) continue;
    off_t sz = lseek(fd, 0, SEEK_END);
    if (sz <= 0) { close(fd); continue; }
    if (want && (size_t)sz != want) { close(fd); continue; }
    matched++;

    void* p = mmap(NULL, sz, PROT_READ, MAP_SHARED, fd, 0);
    if (p == MAP_FAILED) { fprintf(stderr, "fd %s size %ld mmap FAIL\n", e->d_name, (long)sz); close(fd); continue; }

    struct dma_buf_sync s = { .flags = DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ };
    int syncrc = ioctl(fd, DMA_BUF_IOCTL_SYNC, &s);

    const uint8_t* px = (const uint8_t*)p;
    uint64_t sum = 0; uint32_t nz = 0, mn = 255, mx = 0;
    size_t px_n = (size_t)sz; if (px_n > 311296) px_n = 311296;
    for (size_t i = 0; i < px_n; i++) { uint8_t v = px[i]; sum += v; if (v) nz++; if (v<mn) mn=v; if (v>mx) mx=v; }
    double mean = px_n ? (double)sum/px_n : 0;

    printf("fd=%s size=%ld sync=%d  min=%u max=%u mean=%.1f nonzero=%.1f%%  sample:",
           e->d_name, (long)sz, syncrc, mn, mx, mean, 100.0*nz/px_n);
    for (int i = 0; i < 16; i++) printf(" %02x", px[640*240 + 300 + i]); // mid-frame row sample
    printf("\n");

    if (outpfx && (mx > mn)) { // looks like real image content -> save
      char op[128]; snprintf(op, sizeof(op), "%s_fd%s.gray", outpfx, e->d_name);
      int of = open(op, O_WRONLY|O_CREAT|O_TRUNC, 0644);
      if (of >= 0) { if (write(of, p, px_n) < 0) {} close(of); printf("  -> wrote %s (%zu B)\n", op, px_n); }
    }
    s.flags = DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ; ioctl(fd, DMA_BUF_IOCTL_SYNC, &s);
    munmap(p, sz); close(fd);
    if (matched >= 8) break;
  }
  closedir(d);
  if (want == 0) {
    printf("== dmabuf size histogram (pid %d, %d dmabufs) ==\n", pid, dmabufs);
    for (int i=0;i<nh;i++) printf("  size=%zu (0x%zx)  count=%d\n", hs[i], hs[i], hc[i]);
    return 0;
  }
  printf("== dmabufs seen=%d, size-matched=%d ==\n", dmabufs, matched);
  return 0;
}
