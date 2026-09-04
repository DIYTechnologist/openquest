// sb_leech.c -- read the SyncBoss stream *while the sensors HAL owns it*, touching nothing.
//
// notes/29 left one assumption untested: that /dev/syncboss_stream0 is single-open. It has been
// assumed all project long and never checked. It matters because step 2 needs IMU while Meta's
// stack runs -- the HAL must stay up or trackingservice has no poses to compare against.
//
// The published driver says the assumption is wrong. syncboss_stream_open() (syncboss_spi.c) does
// no exclusivity check at all; it just calls miscfifo_fop_open(), which kzalloc's a *per-client*
// kfifo and list_add's it to mf->clients.list. miscfifo_send_buf() then walks that list and copies
// each packet into every client's own fifo. Multiple readers are the design, not an accident.
//
// Two consequences that make this a safe leech, both from the same source:
//   - The stream type filter is per-file (miscfifo_fop_xchg_context on our own fd), so our filter
//     and the HAL's are independent. We set none.
//   - should_send_stream_packet() returns true when context is NULL, so an unfiltered fd receives
//     *every* type -- we see the HAL's traffic without altering what the HAL receives.
//
// Hence: no write() to /dev/syncboss0, no enable packets, no ioctl, no MCU state change. We open
// O_RDONLY and read. If this yields IMU at ~1 kHz with the HAL running, step 2's IMU gap closes
// with no interposition at all.
//
// Framing (notes/13):  01 03 00 <type> 00 <len> <payload[len]>
//
// Build: see build.sh.   Run: sb_leech [seconds] [raw_out]

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define SB_STREAM "/dev/syncboss_stream0"

static double now_s(void) {
  struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec + ts.tv_nsec * 1e-9;
}

struct typestat {
  unsigned long count;
  unsigned char len_seen[8];
  int n_len;
  unsigned char first[64];
  int first_len;
  double t_first, t_last;
};
static struct typestat st[256];

static void note(unsigned char type, const unsigned char *pl, int len, double t) {
  struct typestat *s = &st[type];
  if (!s->count) {
    s->t_first = t;
    s->first_len = len > 64 ? 64 : len;
    memcpy(s->first, pl, s->first_len);
  }
  s->t_last = t;
  s->count++;
  for (int i = 0; i < s->n_len; i++) if (s->len_seen[i] == (unsigned char)len) return;
  if (s->n_len < 8) s->len_seen[s->n_len++] = (unsigned char)len;
}

int main(int argc, char **argv) {
  double secs = argc > 1 ? atof(argv[1]) : 10.0;
  const char *rawpath = argc > 2 ? argv[2] : NULL;

  // The whole experiment is this one line: a second opener while the HAL holds the device.
  int fd = open(SB_STREAM, O_RDONLY);
  if (fd < 0) {
    printf("[-] open %s -> %s (errno %d)\n", SB_STREAM, strerror(errno), errno);
    printf("[=] VERDICT: device refused a second reader -- single-open assumption HOLDS\n");
    return 1;
  }
  printf("[+] open %s -> fd %d  (HAL still holds its own fd)\n", SB_STREAM, fd);

  FILE *raw = rawpath ? fopen(rawpath, "wb") : NULL;
  unsigned char buf[65536];
  static unsigned char acc[262144];
  size_t naccum = 0;
  double t0 = now_s(), t_end = t0 + secs;
  unsigned long total = 0, resync = 0, npkt = 0;
  double t_firstbyte = 0;

  while (now_s() < t_end) {
    struct pollfd pfd = { .fd = fd, .events = POLLIN };
    int pr = poll(&pfd, 1, 200);
    if (pr <= 0) continue;

    ssize_t n = read(fd, buf, sizeof buf);
    if (n <= 0) { if (n < 0 && errno == EINTR) continue; usleep(1000); continue; }
    if (!total) t_firstbyte = now_s();
    total += (unsigned long)n;
    if (raw) fwrite(buf, 1, (size_t)n, raw);
    if (naccum + (size_t)n > sizeof acc) naccum = 0;
    memcpy(acc + naccum, buf, (size_t)n); naccum += (size_t)n;

    size_t i = 0;
    double t = now_s();
    while (i + 6 <= naccum) {
      if (!(acc[i] == 1 && acc[i+1] == 3 && acc[i+2] == 0 && acc[i+4] == 0)) { i++; resync++; continue; }
      unsigned char type = acc[i+3], len = acc[i+5];
      if (i + 6 + len > naccum) break;
      note(type, acc + i + 6, len, t);
      npkt++;
      i += 6 + len;
    }
    memmove(acc, acc + i, naccum - i);
    naccum -= i;
  }
  if (raw) fclose(raw);

  double dur = now_s() - t0;
  printf("\n[stream] %lu bytes, %lu packets in %.1fs (%.1f KB/s), %lu resync bytes\n",
         total, npkt, dur, total / dur / 1024.0, resync);
  if (total) printf("[stream] first byte arrived %.3f s after open\n", t_firstbyte - t0);

  printf("\n%-6s %8s %9s  %-18s %s\n", "type", "count", "rate(Hz)", "payload len(s)", "first payload");
  for (int t = 0; t < 256; t++) {
    if (!st[t].count) continue;
    double d = st[t].t_last - st[t].t_first;
    char lens[64] = "";
    for (int i = 0; i < st[t].n_len; i++)
      snprintf(lens + strlen(lens), sizeof lens - strlen(lens), "%s%u", i ? "," : "", st[t].len_seen[i]);
    printf("0x%02x  %8lu %9.1f  %-18s ", t, st[t].count, d > 0.05 ? st[t].count / d : 0.0, lens);
    for (int i = 0; i < st[t].first_len && i < 24; i++) printf("%02x", st[t].first[i]);
    printf("%s\n", st[t].first_len > 24 ? "..." : "");
  }

  printf("\n[=] VERDICT: %s\n", total
         ? "second reader OPENED and RECEIVED data -- single-open assumption is FALSE"
         : "second reader opened but received NOTHING in the window");
  close(fd);
  return 0;
}
