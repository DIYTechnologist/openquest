// fb_probe.c -- Phase 0 feasibility probe for motion-to-photon (research-notes/62 plan).
//
// Question: does /dev/graphics/fb0 hold the real, live scanout image, and can it be read
// passively (no compositor cooperation, no service stopped) without a plain read()?
//
// A plain read() on this node returns ENODEV -- confirmed on-device before writing this. Standard
// for fbdev drivers that only implement .fb_mmap, not .fb_read (real userspace consumers use
// mmap). So this probe mmaps the buffer instead and repeatedly checksums a fixed strip, printing
// (CLOCK_MONOTONIC, checksum) -- a changing checksum against real head/scene motion is the
// feasibility signal Phase 0 needs; this is not yet the real Phase 1 tap (no vsync interleaving,
// no ROI tuned for the eventual step-detection threshold).
//
// virtual_size (2880x3200) and stride (11520, i.e. 4 bytes/px) come from
// /sys/class/graphics/fb0/{virtual_size,stride} -- read at startup, not hardcoded, since the whole
// point is to stay correct if the panel/driver config differs from what one probe session saw.
// Visible frame height (1600) IS hardcoded from the panel spec already established
// (research-notes/32/44) -- virtual_size's height is a double-buffered total, and there is no
// sysfs field for "visible height" alone.

#define _GNU_SOURCE
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#define VISIBLE_H 1600

static uint64_t now_ns(void) {
  struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000000ull + ts.tv_nsec;
}

static long read_sysfs_long(const char *path) {
  FILE *f = fopen(path, "r");
  if (!f) { perror(path); exit(1); }
  long v; if (fscanf(f, "%ld", &v) != 1) { fprintf(stderr, "bad value in %s\n", path); exit(1); }
  fclose(f);
  return v;
}

int main(int argc, char **argv) {
  double secs = argc > 1 ? atof(argv[1]) : 5.0;
  int period_us = argc > 2 ? atoi(argv[2]) : 2000;

  long vw = 0, vh = 0;
  { FILE *f = fopen("/sys/class/graphics/fb0/virtual_size", "r");
    if (!f) { perror("virtual_size"); return 1; }
    if (fscanf(f, "%ld,%ld", &vw, &vh) != 2) { fprintf(stderr, "bad virtual_size\n"); return 1; }
    fclose(f); }
  long stride = read_sysfs_long("/sys/class/graphics/fb0/stride");
  size_t maplen = (size_t)stride * (size_t)vh;
  fprintf(stderr, "[+] fb0: virtual %ldx%ld stride=%ld maplen=%zu\n", vw, vh, stride, maplen);

  int fd = open("/dev/graphics/fb0", O_RDONLY);
  if (fd < 0) { perror("open fb0"); return 1; }

  void *base = mmap(NULL, maplen, PROT_READ, MAP_SHARED, fd, 0);
  if (base == MAP_FAILED) { perror("mmap fb0"); return 1; }
  fprintf(stderr, "[+] mmap ok at %p\n", base);

  if (period_us < 0) {
    // One-shot full-buffer scan: nonzero byte count per 100-row band, to find out whether ANY of
    // the mapped buffer has real content, before assuming one probed row's zero checksum means
    // the mapping is empty rather than just sampling a dark part of the scene.
    const unsigned char *b = (const unsigned char *)base;
    for (long row = 0; row < vh; row += 100) {
      unsigned long nz = 0;
      size_t off = (size_t)row * (size_t)stride;
      size_t band = (size_t)100 * (size_t)stride;
      if (off + band > maplen) band = maplen - off;
      for (size_t i = 0; i < band; i += 4) if (b[off + i]) nz++;
      fprintf(stderr, "rows %4ld-%4ld: nonzero=%lu/%zu\n", row, row + 100, nz, band / 4);
    }
    munmap(base, maplen);
    close(fd);
    return 0;
  }

  // A horizontal strip through the middle of the CURRENTLY VISIBLE frame. pan.y tells us which
  // vertical offset (in the double-buffered virtual fb) is on screen right now -- read once here,
  // not proof against the compositor flipping buffers mid-run, but sufficient to test whether
  // content changes at all.
  long pan_y = 0;
  { FILE *f = fopen("/sys/class/graphics/fb0/pan", "r");
    if (f) { long px; if (fscanf(f, "%ld,%ld", &px, &pan_y) != 2) pan_y = 0; fclose(f); } }
  size_t row_off = (size_t)stride * (size_t)(pan_y + VISIBLE_H / 2);
  const unsigned char *strip = (const unsigned char *)base + row_off;
  size_t strip_len = (size_t)stride;
  if (row_off + strip_len > maplen) { fprintf(stderr, "[-] strip out of range\n"); return 1; }
  fprintf(stderr, "[+] sampling strip at byte offset %zu, pan_y=%ld\n", row_off, pan_y);

  uint64_t t_end_ns = now_ns() + (uint64_t)(secs * 1e9);
  uint32_t last_sum = 0;
  int changes = 0, n = 0;
  printf("#mono_ns,checksum\n");
  while (now_ns() < t_end_ns) {
    uint32_t sum = 0;
    for (size_t i = 0; i < strip_len; i += 16) sum = sum * 131 + strip[i];
    printf("%llu,%u\n", (unsigned long long)now_ns(), sum);
    if (n > 0 && sum != last_sum) changes++;
    last_sum = sum;
    n++;
    usleep(period_us);
  }
  fprintf(stderr, "[+] %d samples, %d changed vs previous sample (%.1f%%)\n",
          n, changes, n > 1 ? 100.0 * changes / (n - 1) : 0.0);

  munmap(base, maplen);
  close(fd);
  return 0;
}
