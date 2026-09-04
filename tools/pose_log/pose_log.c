// pose_log.c — log Meta's head poses at frame rate, for ground truth (notes/18 step 2).
//
// `trackinginterface_cli getHeadTrackingData` manages 3.3 Hz: each call is a process spawn plus
// Binder setup, and the binary statically links its tracking client so there is no library API to
// borrow. The poses live in a shared-memory double buffer, so read them straight out of it.
//
// Region (notes/28):
//   /dev/ashmem/TrackingServiceHeadTracker, 8 KB, mapped rw-s by trackingservice
//     0x00  u32 nslots            (observed 2)
//     0x10  u32 seq               (mirrored at 0x14; increments once per published pose)
//     0x18  slot[0]               stride 0xa0
//       +0x10  float32 quat[4]    x, y, z, w   <-- xyzw, NOT the wxyz the CLI prints
//       +0x20  float32 pos[3]     x, y, z
//
// Read via /proc/<pid>/mem rather than mapping the ashmem fd: the fd would have to come from
// ITrackingService::getSharedMemoryFileDescriptor over Binder, and for a measurement tool a
// read-only pread of another process is simpler and has no chance of perturbing the producer.
// The region address is NOT stable across restarts of trackingservice, so it is resolved by name
// from /proc/<pid>/maps every run.
//
// Usage: pose_log [hz] [seconds] > poses.csv

#define _GNU_SOURCE
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define REGION "TrackingServiceHeadTracker"
#define OFF_NSLOTS 0x00
#define OFF_SEQ    0x10
#define OFF_SLOT0  0x18
#define SLOT_STRIDE 0xa0
#define IN_QUAT    0x10
#define IN_POS     0x20

static volatile sig_atomic_t g_stop = 0;
static void on_sig(int s) { (void)s; g_stop = 1; }

static double now_s(void) {
  struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec + ts.tv_nsec * 1e-9;
}

static int find_pid(const char *name) {
  FILE *f;
  char path[64], comm[128];
  for (int pid = 1; pid < 65536; pid++) {
    snprintf(path, sizeof path, "/proc/%d/comm", pid);
    f = fopen(path, "r");
    if (!f) continue;
    if (fgets(comm, sizeof comm, f)) {
      comm[strcspn(comm, "\n")] = 0;
      if (!strcmp(comm, name)) { fclose(f); return pid; }
    }
    fclose(f);
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
    if (sscanf(line, "%llx-%llx", &s, &e) == 2) {
      *base = s; *end = e; fclose(f); return 0;
    }
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
  char mp[64]; snprintf(mp, sizeof mp, "/proc/%d/mem", pid);
  int fd = open(mp, O_RDONLY);
  if (fd < 0) { perror(mp); return 1; }
  fprintf(stderr, "[+] trackingservice pid=%d  %s @ 0x%llx (%llu B)\n",
          pid, REGION, base, end - base);

  uint32_t nslots = 0;
  if (pread(fd, &nslots, 4, (off_t)(base + OFF_NSLOTS)) != 4 || nslots == 0 || nslots > 64) {
    fprintf(stderr, "[-] implausible nslots=%u\n", nslots);
    return 1;
  }
  fprintf(stderr, "[+] nslots=%u stride=0x%x\n", nslots, SLOT_STRIDE);

  printf("#host_mono_ns,seq,pos_x,pos_y,pos_z,quat_x,quat_y,quat_z,quat_w\n");

  const double period = 1.0 / hz;
  double t0 = now_s(), next = t0;
  long n = 0, nnew = 0, nerr = 0;
  uint32_t last_seq = 0xffffffffu;

  while (!g_stop && now_s() - t0 < secs) {
    uint32_t seq = 0;
    if (pread(fd, &seq, 4, (off_t)(base + OFF_SEQ)) != 4) { nerr++; goto wait; }
    {
      unsigned long long slot = base + OFF_SLOT0 + (unsigned long long)(seq % nslots) * SLOT_STRIDE;
      float q[4], p[3];
      if (pread(fd, q, sizeof q, (off_t)(slot + IN_QUAT)) != sizeof q) { nerr++; goto wait; }
      if (pread(fd, p, sizeof p, (off_t)(slot + IN_POS)) != sizeof p) { nerr++; goto wait; }
      struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
      printf("%llu,%u,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g\n",
             (unsigned long long)ts.tv_sec * 1000000000ull + ts.tv_nsec, seq,
             p[0], p[1], p[2], q[0], q[1], q[2], q[3]);
      n++;
      if (seq != last_seq) { nnew++; last_seq = seq; }
    }
  wait:
    next += period;
    double slack = next - now_s();
    if (slack > 0) usleep((useconds_t)(slack * 1e6));
    else next = now_s();
  }

  double dur = now_s() - t0;
  fprintf(stderr, "[+] %ld samples in %.2f s (%.1f Hz), %ld distinct seq (%.1f Hz new poses), %ld read errors\n",
          n, dur, n / dur, nnew, nnew / dur, nerr);
  close(fd);
  return 0;
}
