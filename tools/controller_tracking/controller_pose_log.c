// controller_pose_log.c — log a Touch controller's own tracked pose at frame rate, for ground
// truth (research-notes/56's open item: pose_log.c only reads TrackingServiceHeadTracker).
//
// TrackingServiceController (research-notes/19, research-notes/24 -- noted as a candidate,
// research-notes/56 -- discovered and read for the first time) was never probed via injection:
// updateRemotePoseField's parcel layout was verified BIT-FOR-BIT correct against the real Bp proxy
// disassembly (transaction code 1, writeInterfaceToken/writeString16(id)/writeInt32(field)/
// writeFloatVector), yet the service still rejects it (accepted=false, no exception) -- a
// server-side precondition inside trackingservice/libtrackingengines.so, which research-notes/01
// already ruled out analyzing. So this reads the REAL, live-tracked pose instead of an injected
// synthetic one, discovered by cross-referencing a `dumpsys tracking` reading (2-3s latency, 2
// decimal digits -- not enough for an exact byte match) against a full raw dump of the 16 KB
// region, matched by tolerance rather than exact bytes.
//
// Layout found empirically, structurally identical in its per-slot shape to
// TrackingServiceHeadTracker (research-notes/28) -- quat then pos at the same relative offsets --
// but a different, larger ring (~34 slots vs 2) and a different stride:
//   slot: +0x10  float32 quat[4]   x, y, z, w   (verified against a live dumpsys reading, exact
//                                                 to the printed precision)
//   slot: +0x20  float32 pos[3]    x, y, z      (same)
//   stride 0xa8 (unlike head tracker's 0xa0 -- more trailing fields per slot, not decoded)
//
// The exact ring boundary (which byte range belongs to which of the two controllers, and the
// seq/timestamp field's semantics) is NOT decoded -- both would need more probing this session
// didn't have room for. Instead: auto-detect the ring at startup by scanning the whole region for
// the longest run of slot-stride-spaced unit-norm quaternions (self-calibrating, so it survives
// the region's base address moving across restarts the same way pose_log.c's by-name resolution
// already handles), then poll all slots in that run and emit whichever one's bytes changed since
// the last poll -- diff-based freshness instead of a decoded sequence counter.
//
// Usage: controller_pose_log [hz] [seconds] > poses.csv

#define _GNU_SOURCE
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define REGION "TrackingServiceController"
#define REGION_SIZE 0x4000
#define STRIDE 0xa8
#define IN_QUAT 0x10
#define IN_POS  0x20
#define MAX_SLOTS (REGION_SIZE / STRIDE + 1)
#define MIN_RUN 15  // real ring has consistently been 30+ slots; false positives seen so far <= 7

static volatile sig_atomic_t g_stop = 0;
static void on_sig(int s) { (void)s; g_stop = 1; }

static double now_s(void) {
  struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec + ts.tv_nsec * 1e-9;
}

static int find_pid(const char *name) {
  for (int pid = 1; pid < 65536; pid++) {
    char path[64], comm[128];
    snprintf(path, sizeof path, "/proc/%d/comm", pid);
    FILE *f = fopen(path, "r");
    if (!f) continue;
    int hit = 0;
    if (fgets(comm, sizeof comm, f)) {
      comm[strcspn(comm, "\n")] = 0;
      hit = !strcmp(comm, name);
    }
    fclose(f);
    if (hit) return pid;
  }
  return -1;
}

static int find_region(int pid, unsigned long long *base, unsigned long long *end) {
  char path[64]; snprintf(path, sizeof path, "/proc/%d/maps", pid);
  FILE *f = fopen(path, "r");
  if (!f) return -1;
  char line[512];
  while (fgets(line, sizeof line, f)) {
    if (!strstr(line, REGION)) continue;
    unsigned long long s, e;
    if (sscanf(line, "%llx-%llx", &s, &e) == 2) { *base = s; *end = e; fclose(f); return 0; }
  }
  fclose(f);
  return -1;
}

int main(int argc, char **argv) {
  double hz = argc > 1 ? atof(argv[1]) : 60.0;
  double secs = argc > 2 ? atof(argv[2]) : 10.0;
  if (hz <= 0) hz = 60.0;
  signal(SIGINT, on_sig); signal(SIGTERM, on_sig);

  int pid = find_pid("trackingservice");
  if (pid < 0) { fprintf(stderr, "[-] trackingservice not found\n"); return 1; }
  unsigned long long base = 0, end = 0;
  if (find_region(pid, &base, &end) < 0) {
    fprintf(stderr, "[-] %s not mapped in pid %d\n", REGION, pid);
    return 1;
  }
  unsigned long long region_size = end - base;
  char mp[64]; snprintf(mp, sizeof mp, "/proc/%d/mem", pid);
  int fd = open(mp, O_RDONLY);
  if (fd < 0) { perror(mp); return 1; }
  fprintf(stderr, "[+] trackingservice pid=%d  %s @ 0x%llx (%llu B)\n", pid, REGION, base, region_size);

  unsigned char *buf = malloc(region_size);

  // Auto-detect: longest run of slot-stride-spaced candidates, where a candidate is a unit-norm
  // quaternion AND a plausible (nonzero, <2m) position at the same hypothetical slot start.
  //
  // Two bugs found and fixed while bringing this up, both worth recording since either alone
  // silently produces a wrong-but-plausible-looking answer:
  //  1. Scanning EVERY 4-byte offset (not just true slot boundaries) means a single spurious
  //     "looks valid" reading at the WRONG phase, sitting between two real slots, breaks a single
  //     running chain even though the true slots either side of it are fine. A candidate must be
  //     checked for chain membership by exact-offset SET LOOKUP (any s, s+STRIDE, s+2*STRIDE, ...),
  //     not by "did the immediately-preceding scan step continue the chain".
  //  2. Quat-norm alone is not a strong enough filter: an identity quaternion (0,0,0,1) has norm
  //     EXACTLY 1.0 and occurs constantly in zero-initialized/padding memory, producing long,
  //     completely spurious "runs" of all-zero positions at the wrong phase. Requiring the
  //     position to also be nonzero and within a plausible hand-held range eliminates these.
  //
  // A third, operational gotcha found running this for real: the ring only holds history while
  // the controller is actively tracked, and gets sparse/empty again within a couple of seconds of
  // it going idle. A capture script that starts this tool before the controller is actually up and
  // moving finds nothing on a single attempt. Retry for a while rather than fail fast -- this is
  // meant to run unattended alongside a capture script, not be re-launched by hand each time.
  int best_start = -1, best_len = 0;
  double detect_deadline = now_s() + 10.0;
  while (now_s() < detect_deadline) {
    if (pread(fd, buf, region_size, (off_t)base) != (ssize_t)region_size) {
      fprintf(stderr, "[-] region read failed\n"); return 1;
    }
    char *is_cand = calloc((size_t)(region_size / 4) + 1, 1);  // indexed by byte-offset/4
    for (unsigned long long s = 0; s + STRIDE <= region_size; s += 4) {
      float q[4]; memcpy(q, buf + s + IN_QUAT, sizeof q);
      double qn = q[0]*(double)q[0] + q[1]*(double)q[1] + q[2]*(double)q[2] + q[3]*(double)q[3];
      if (!(qn > 0.99 && qn < 1.01)) continue;
      float p[3]; memcpy(p, buf + s + IN_POS, sizeof p);
      double pn = p[0]*(double)p[0] + p[1]*(double)p[1] + p[2]*(double)p[2];
      if (pn < 1e-6 || pn > 4.0) continue;
      is_cand[s / 4] = 1;
    }
    // Collect every run >= MIN_RUN, not just the longest one, and check each (longest first) for
    // genuine VARIATION across its slots before trusting it. Length and a plausible-looking value
    // both turned out insufficient on their own: a repeating (0,1,0,0) quaternion -- some other,
    // unrelated fixed-value structure that happens to also be unit-norm and to recur at this same
    // stride elsewhere in the region -- produced an 18-slot "run" with an EXACTLY constant
    // quaternion at every slot. Real tracked motion does not hold still to the bit; requiring the
    // quaternion to actually vary across the run is what a length/plausibility check alone misses.
    for (unsigned long long s = 0; s + STRIDE <= region_size; s += 4) {
      if (!is_cand[s / 4]) continue;
      if (s >= STRIDE && is_cand[(s - STRIDE) / 4]) continue;  // not a chain start
      int len = 1;
      while (s + (unsigned long long)len * STRIDE + STRIDE <= region_size &&
             is_cand[(s + (unsigned long long)len * STRIDE) / 4]) len++;
      if (len < MIN_RUN || len <= best_len) continue;
      float q0[4]; memcpy(q0, buf + s + IN_QUAT, sizeof q0);
      double spread = 0;
      for (int i = 1; i < len; i++) {
        float qi[4]; memcpy(qi, buf + s + (unsigned long long)i * STRIDE + IN_QUAT, sizeof qi);
        for (int k = 0; k < 4; k++) { double d = qi[k] - q0[k]; spread += d * d; }
      }
      if (spread < 1e-6) {
        fprintf(stderr, "[.] rejecting %d-slot run at +0x%llx: quaternion never varies "
                        "(not real tracked motion)\n", len, s);
        continue;
      }
      best_len = len; best_start = (int)s;
    }
    free(is_cand);
    if (best_len >= MIN_RUN) break;
    fprintf(stderr, "[.] no ring yet (best run %d slots, need >= %d) -- retrying, controller may "
                    "not be active/moving for long enough yet\n", best_len, MIN_RUN);
    best_len = 0; best_start = -1;
    usleep(500000);
  }
  if (best_len < MIN_RUN) {
    fprintf(stderr, "[-] no plausible slot ring found after retrying for 10s\n");
    return 1;
  }
  if (best_len > MAX_SLOTS) best_len = MAX_SLOTS;
  fprintf(stderr, "[+] detected ring: base+0x%x, %d slots, stride 0x%x\n", best_start, best_len, STRIDE);

  unsigned char *prev = malloc((size_t)best_len * STRIDE);
  memcpy(prev, buf + best_start, (size_t)best_len * STRIDE);
  free(buf);

  printf("#host_mono_ns,slot,pos_x,pos_y,pos_z,quat_x,quat_y,quat_z,quat_w\n");

  const double period = 1.0 / hz;
  double t0 = now_s(), next = t0;
  long n = 0, nerr = 0;

  while (!g_stop && now_s() - t0 < secs) {
    unsigned char cur[MAX_SLOTS * STRIDE];
    size_t want = (size_t)best_len * STRIDE;
    if (pread(fd, cur, want, (off_t)(base + best_start)) != (ssize_t)want) {
      nerr++; goto wait;
    }
    for (int i = 0; i < best_len; i++) {
      unsigned char *a = cur + (size_t)i * STRIDE, *b = prev + (size_t)i * STRIDE;
      if (memcmp(a, b, STRIDE) == 0) continue;
      float q[4], p[3];
      memcpy(q, a + IN_QUAT, sizeof q);
      memcpy(p, a + IN_POS, sizeof p);
      struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
      printf("%llu,%d,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g\n",
             (unsigned long long)ts.tv_sec * 1000000000ull + ts.tv_nsec, i,
             p[0], p[1], p[2], q[0], q[1], q[2], q[3]);
      n++;
    }
    memcpy(prev, cur, want);
  wait:
    next += period;
    double slack = next - now_s();
    if (slack > 0) usleep((useconds_t)(slack * 1e6)); else next = now_s();
  }

  double dur = now_s() - t0;
  fprintf(stderr, "[+] %ld changed-slot samples in %.2f s (%.1f Hz), %ld read errors\n",
          n, dur, n / dur, nerr);
  close(fd);
  return 0;
}
