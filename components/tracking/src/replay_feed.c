// replay_feed.c — replay a recorded cam_kernel capture as a live stream.
//
// Lets the live consumer (vio_live) be validated against a capture that contains REAL MOTION,
// without needing anyone to pick the headset up. The live path is otherwise untestable on a desk:
// OpenVINS gates initialisation on IMU excitation, so a stationary capture never initialises and
// proves nothing about poses.
//
// Emits exactly the record format cam_kernel writes with outdir "-", interleaving IMU and camera
// records in timestamp order so the consumer sees them as it would live.
//
// Usage: replay_feed <capture_dir> [speed]   # speed 0 = as fast as possible (default)
//        replay_feed /data/local/tmp/vio-b2 | vio_live cfg.yaml imu_rect.txt --no-inject

#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define W 640
#define H 481
#define FRAME_SZ (W * H)

#pragma pack(push, 1)
struct rec { uint8_t type, cam; uint16_t w, h; uint64_t dev_ns, host_ns; uint32_t bytes; };
#pragma pack(pop)

struct fr { uint64_t ts; int cam; char file[64]; };
struct im { uint64_t dev_ns, host_ns; float v[6]; };
struct ev { uint64_t host_ns, dev_ns; int kind; int idx; };   // kind 0=imu 1=expo 2=cam
struct chunk { uint64_t host_ns, off, bytes; };

static int cmp_ev(const void *a, const void *b) {
  uint64_t x = ((const struct ev *)a)->host_ns, y = ((const struct ev *)b)->host_ns;
  return x < y ? -1 : x > y ? 1 : 0;
}

static int cmp_fr(const void *a, const void *b) {
  uint64_t x = ((const struct fr *)a)->ts, y = ((const struct fr *)b)->ts;
  return x < y ? -1 : x > y ? 1 : 0;
}

int main(int argc, char **argv) {
  if (argc < 2) { fprintf(stderr, "usage: %s <capture_dir>\n", argv[0]); return 2; }
  const char *dir = argv[1];
  char p[512];

  // ---- frames.csv ----
  snprintf(p, sizeof p, "%s/frames.csv", dir);
  FILE *f = fopen(p, "r");
  if (!f) { perror(p); return 1; }
  int fcap = 8192, fn = 0;
  struct fr *fr = malloc((size_t)fcap * sizeof *fr);
  char line[512];
  while (fgets(line, sizeof line, f)) {
    if (line[0] == '#') continue;
    int seq, cam; unsigned long long ts; char name[64];
    if (sscanf(line, "%d,%d,%llu,%63s", &seq, &cam, &ts, name) != 4) continue;
    if (fn == fcap) { fcap *= 2; fr = realloc(fr, (size_t)fcap * sizeof *fr); }
    fr[fn].ts = ts; fr[fn].cam = cam;
    snprintf(fr[fn].file, sizeof fr[fn].file, "%s", name);
    fn++;
  }
  fclose(f);
  qsort(fr, (size_t)fn, sizeof *fr, cmp_fr);

  // ---- syncboss.raw -> IMU records ----
  snprintf(p, sizeof p, "%s/syncboss.raw", dir);
  f = fopen(p, "rb");
  if (!f) { perror(p); return 1; }
  fseek(f, 0, SEEK_END); long rn = ftell(f); fseek(f, 0, SEEK_SET);
  unsigned char *raw = malloc((size_t)rn);
  if (fread(raw, 1, (size_t)rn, f) != (size_t)rn) { fprintf(stderr, "short read\n"); return 1; }
  fclose(f);

  // syncboss_chunks.csv gives the host monotonic time at which each byte range arrived. Using it
  // means the replay reproduces the real dev/host relationship, so the consumer's clock logic is
  // genuinely exercised instead of being handed a synthetic timeline that hides its bugs.
  snprintf(p, sizeof p, "%s/syncboss_chunks.csv", dir);
  FILE *cf = fopen(p, "r");
  int ccap = 8192, cn = 0;
  struct chunk *ch = malloc((size_t)ccap * sizeof *ch);
  if (cf) {
    while (fgets(line, sizeof line, cf)) {
      if (line[0] == '#') continue;
      unsigned long long h, o, b;
      if (sscanf(line, "%llu,%llu,%llu", &h, &o, &b) != 3) continue;
      if (cn == ccap) { ccap *= 2; ch = realloc(ch, (size_t)ccap * sizeof *ch); }
      ch[cn].host_ns = h; ch[cn].off = o; ch[cn].bytes = b; cn++;
    }
    fclose(cf);
  }
  fprintf(stderr, "[replay] %d syncboss chunks\n", cn);

  int icap = 262144, in = 0;
  struct im *imu = malloc((size_t)icap * sizeof *imu);
  int ecap = 8192, en = 0;
  uint64_t *ex = malloc((size_t)ecap * sizeof *ex);
  uint64_t *exh = malloc((size_t)ecap * sizeof *exh);
  int ci = 0;
  for (long i = 0; i + 6 <= rn; ) {
    if (!(raw[i] == 1 && raw[i+1] == 3 && raw[i+2] == 0 && raw[i+4] == 0)) { i++; continue; }
    while (ci + 1 < cn && (uint64_t)i >= ch[ci].off + ch[ci].bytes) ci++;
    uint64_t pkt_host = cn ? ch[ci].host_ns : 0;
    unsigned char t = raw[i+3], L = raw[i+5];
    if (i + 6 + L > rn) break;
    if (t == 0xe0 && L == 14) {
      uint32_t eus; memcpy(&eus, raw + i + 6 + 1, 4);
      if (en == ecap) { ecap *= 2; ex = realloc(ex, (size_t)ecap * sizeof *ex); }
      ex[en] = (uint64_t)eus * 1000ull; exh[en] = pkt_host; en++;
    }
    if (t == 0x50 && L == 36) {
      const unsigned char *pl = raw + i + 6;
      uint32_t us; memcpy(&us, pl, 4);
      float v[7]; memcpy(v, pl + 8, 28);
      if (in == icap) { icap *= 2; imu = realloc(imu, (size_t)icap * sizeof *imu); }
      imu[in].dev_ns = (uint64_t)us * 1000ull;
      imu[in].host_ns = pkt_host;
      imu[in].v[0]=v[3]; imu[in].v[1]=v[4]; imu[in].v[2]=v[5];   // gyro deg/s
      imu[in].v[3]=v[0]; imu[in].v[4]=v[1]; imu[in].v[5]=v[2];   // accel g
      in++;
    }
    i += 6 + L;
  }
  fprintf(stderr, "[replay] %d frames, %d imu samples, %d exposure stamps\n", fn, in, en);
  if (!fn || !in) return 1;

  // Camera stamps are CLOCK_MONOTONIC; IMU stamps are the nRF clock with a different epoch. The
  // live consumer recovers the mapping from the host_ns each record carries, so reproduce that
  // here: assign every record a host time on the CAMERA timeline, and let the consumer rediscover
  // the IMU offset exactly as it must live.
  // Merge everything onto the real host timeline and emit in arrival order, exactly as the live
  // capture would produce it.
  int nev = in + en + fn, ne = 0;
  struct ev *evs = malloc((size_t)nev * sizeof *evs);
  for (int k = 0; k < in; k++) evs[ne++] = (struct ev){ imu[k].host_ns, imu[k].dev_ns, 0, k };
  for (int k = 0; k < en; k++) evs[ne++] = (struct ev){ exh[k], ex[k], 1, k };
  for (int k = 0; k < fn; k++) evs[ne++] = (struct ev){ fr[k].ts, fr[k].ts, 2, k };
  qsort(evs, (size_t)ne, sizeof *evs, cmp_ev);

  struct rec r;
  unsigned char *px = malloc(FRAME_SZ);
  long ni = 0, nx = 0, nc = 0;
  for (int k = 0; k < ne; k++) {
    struct ev *e = &evs[k];
    if (e->kind == 0) {
      r = (struct rec){ 'I', 0, 0, 0, e->dev_ns, e->host_ns, 24 };
      fwrite(&r, sizeof r, 1, stdout);
      fwrite(imu[e->idx].v, 1, 24, stdout);
      ni++;
    } else if (e->kind == 1) {
      r = (struct rec){ 'E', 0, 0, 0, e->dev_ns, e->host_ns, 0 };
      fwrite(&r, sizeof r, 1, stdout);
      nx++;
    } else {
      snprintf(p, sizeof p, "%s/raw/%s", dir, fr[e->idx].file);
      FILE *g = fopen(p, "rb");
      if (!g) continue;
      size_t got = fread(px, 1, FRAME_SZ, g);
      fclose(g);
      if (got != FRAME_SZ) continue;
      r = (struct rec){ 'C', (uint8_t)fr[e->idx].cam, W, H, e->dev_ns, e->host_ns, FRAME_SZ };
      fwrite(&r, sizeof r, 1, stdout);
      fwrite(px, 1, FRAME_SZ, stdout);
      nc++;
    }
  }
  fflush(stdout);
  fprintf(stderr, "[replay] emitted imu=%ld expo=%ld cam=%ld\n", ni, nx, nc);
  return 0;
}
