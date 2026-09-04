// sb_survey.c — catalogue every packet type the SyncBoss MCU emits, with no Meta blobs.
//
// Step 3 prep (notes/18 track D): we already decode type 0x50 (IMU, 1 kHz) and 0x51 (camera
// exposure, 30 Hz) in tools/vio/sb_decode.py. Controllers reach the SoC over the same MCU, so the
// packet types carrying button/IMU/pose data are in this stream -- but nothing has ever enumerated
// what else is in there.
//
// The stream device emits NOTHING until an MCU session is open: a bare `cat /dev/syncboss_stream0`
// returns 0 bytes. queue_tx_packet() in the published driver snoops writes to /dev/syncboss0 and
// type 0x28 (40) is what starts the session, so we open the gate ourselves exactly as cam_kernel
// does, then read.
//
// Framing (from notes/13, confirmed against sb_decode.py):
//     01 03 00 <type> 00 <len> <payload[len]>
//
// Build: see build.sh.   Run: sb_survey [seconds] [raw_out]

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define SB_DEV    "/dev/syncboss0"
#define SB_STREAM "/dev/syncboss_stream0"

static int sb_fd = -1, sb_stream_fd = -1;
static unsigned char sb_seq = 0xa0;

static double now_s(void) {
  struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec + ts.tv_nsec * 1e-9;
}

static int sb_send(const char *what, unsigned char type, unsigned char seq,
                   const void *data, unsigned char len) {
  unsigned char pkt[64];
  pkt[0] = type; pkt[1] = seq; pkt[2] = len;
  if (len) memcpy(pkt + 3, data, len);
  ssize_t n = write(sb_fd, pkt, (size_t)len + 3);
  printf("[%c] mcu %-24s type=0x%02x -> %zd\n", n == (ssize_t)len + 3 ? '+' : '-', what, type, n);
  return n == (ssize_t)len + 3 ? 0 : -1;
}

// Per-type accounting. Types are a byte, so a flat table is simplest and avoids any allocation in
// the read loop.
struct typestat {
  unsigned long count;
  unsigned char len_seen[8];   // distinct payload lengths observed
  int n_len;
  unsigned char first[64];     // first payload, for eyeballing structure
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
  double secs = argc > 1 ? atof(argv[1]) : 20.0;
  const char *rawpath = argc > 2 ? argv[2] : "/data/local/tmp/sb_raw.bin";

  sb_stream_fd = open(SB_STREAM, O_RDONLY);
  printf("[%c] open %s -> fd %d\n", sb_stream_fd < 0 ? '-' : '+', SB_STREAM, sb_stream_fd);
  if (sb_stream_fd < 0) { perror("open " SB_STREAM); return 1; }
  sb_fd = open(SB_DEV, O_RDWR);
  printf("[%c] open %s -> fd %d\n", sb_fd < 0 ? '-' : '+', SB_DEV, sb_fd);
  if (sb_fd < 0) { perror("open " SB_DEV); return 1; }

  unsigned char none = 0;
  sb_send("camera_probe(session on)", 0x28, 0, &none, 0);
  usleep(200000);

  FILE *raw = fopen(rawpath, "wb");
  unsigned char buf[65536];
  unsigned char acc[131072];
  size_t naccum = 0;
  double t_end = now_s() + secs;
  unsigned long total = 0, resync = 0;

  while (now_s() < t_end) {
    ssize_t n = read(sb_stream_fd, buf, sizeof buf);
    if (n <= 0) { if (n < 0 && errno == EINTR) continue; usleep(1000); continue; }
    total += (unsigned long)n;
    if (raw) fwrite(buf, 1, (size_t)n, raw);
    if (naccum + (size_t)n > sizeof acc) naccum = 0;      // overflow: drop, don't corrupt
    memcpy(acc + naccum, buf, (size_t)n); naccum += (size_t)n;

    size_t i = 0;
    double t = now_s();
    while (i + 6 <= naccum) {
      if (!(acc[i] == 1 && acc[i+1] == 3 && acc[i+2] == 0 && acc[i+4] == 0)) { i++; resync++; continue; }
      unsigned char type = acc[i+3], len = acc[i+5];
      if (i + 6 + len > naccum) break;                    // partial frame: wait for more
      note(type, acc + i + 6, len, t);
      i += 6 + len;
    }
    memmove(acc, acc + i, naccum - i);
    naccum -= i;
  }
  if (raw) fclose(raw);

  printf("\n[stream] %lu bytes in %.1fs (%.1f KB/s), %lu resync bytes\n",
         total, secs, total / secs / 1024.0, resync);
  printf("\n%-6s %8s %9s  %-18s %s\n", "type", "count", "rate(Hz)", "payload len(s)", "first payload");
  for (int t = 0; t < 256; t++) {
    if (!st[t].count) continue;
    double dur = st[t].t_last - st[t].t_first;
    char lens[64] = "";
    for (int i = 0; i < st[t].n_len; i++)
      snprintf(lens + strlen(lens), sizeof lens - strlen(lens), "%s%u", i ? "," : "", st[t].len_seen[i]);
    printf("0x%02x  %8lu %9.1f  %-18s ", t, st[t].count,
           dur > 0.05 ? st[t].count / dur : 0.0, lens);
    for (int i = 0; i < st[t].first_len && i < 24; i++) printf("%02x", st[t].first[i]);
    printf("%s\n", st[t].first_len > 24 ? "..." : "");
  }

  // Leave the MCU as we found it.
  sb_send("camera_release(session off)", 0x29, sb_seq++, &none, 0);
  close(sb_fd); close(sb_stream_fd);
  return 0;
}
