// pose_inject.c — drive Meta's tracker from our poses, at frame rate, with no Meta libraries.
//
// Step 4 (notes/18, notes/23). `service call` proved the interface accepts external poses, but each
// invocation is a process spawn (~21 ms) and a pose needs TWO calls (position + orientation), so it
// tops out around 23 Hz -- under frame rate. This is the same transactions from one process.
//
// Uses the STABLE NDK binder C API (libbinder_ndk, Android 10+), not libbinder's C++ ABI, so
// nothing here depends on a Meta library or on matching a C++ ABI we do not control. One symbol
// (AServiceManager_getService) is resolved at runtime rather than linked -- see below.
//
// Interface (transaction codes recovered in notes/23 by disassembling libossdk's Bp proxies):
//   oculus.internal.virtual_input.ITrackingDataInjectionService
//     2 = updateHeadsetPoseField(int field, float[] values) -> bool
//         field 0 = position   {x,y,z}
//         field 1 = orientation quaternion {x,y,z,w}  <-- xyzw on the wire; the CLI PRINTS wxyz
//
// Build: see build.sh.
// Run:   pose_inject <traj.txt> [hz]     replay a TUM-format trajectory (t x y z qx qy qz qw)
//        pose_inject --circle [hz] [sec] synthetic motion, for a standalone check

#define _GNU_SOURCE
#include <errno.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <android/binder_ibinder.h>
#include <android/binder_parcel.h>
#include <android/binder_status.h>
#include <dlfcn.h>

// AServiceManager_getService is a stable versioned export of libbinder_ndk.so, but it lives in
// <android/binder_manager.h>, which the NDK does not ship, and the NDK's *stub* libbinder_ndk does
// not export it either -- so it cannot be linked, only resolved at runtime against the real device
// library. Everything else we use is in the NDK stub and links normally.
typedef AIBinder *(*get_service_fn)(const char *);
static get_service_fn AServiceManager_getService;

static int bind_service_manager(void) {
  void *h = dlopen("libbinder_ndk.so", RTLD_NOW);
  if (!h) { fprintf(stderr, "[-] dlopen libbinder_ndk.so: %s\n", dlerror()); return -1; }
  AServiceManager_getService = (get_service_fn)dlsym(h, "AServiceManager_getService");
  if (!AServiceManager_getService) {
    fprintf(stderr, "[-] dlsym AServiceManager_getService: %s\n", dlerror());
    return -1;
  }
  return 0;
}

#define SVC   "TrackingDataInjection"
#define IFACE "oculus.internal.virtual_input.ITrackingDataInjectionService"
#define TX_UPDATE_HEADSET_POSE_FIELD 2
#define FIELD_POSITION    0
#define FIELD_ORIENTATION 1

static volatile sig_atomic_t g_stop = 0;
static void on_sig(int s) { (void)s; g_stop = 1; }

static double now_s(void) {
  struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec + ts.tv_nsec * 1e-9;
}

// The class exists only so prepareTransaction writes the right interface token; we never receive
// transactions, so the callbacks are inert.
static void *cls_on_create(void *args) { return args; }
static void cls_on_destroy(void *ud) { (void)ud; }
static binder_status_t cls_on_transact(AIBinder *b, transaction_code_t c, const AParcel *in,
                                       AParcel *out) {
  (void)b; (void)c; (void)in; (void)out; return STATUS_UNKNOWN_TRANSACTION;
}

static AIBinder *g_svc = NULL;

static int inject_field(int field, const float *v, int n) {
  AParcel *in = NULL, *out = NULL;
  binder_status_t st = AIBinder_prepareTransaction(g_svc, &in);
  if (st != STATUS_OK) { fprintf(stderr, "[-] prepareTransaction: %d\n", st); return -1; }
  if (AParcel_writeInt32(in, field) != STATUS_OK) return -1;
  if (AParcel_writeFloatArray(in, v, n) != STATUS_OK) return -1;
  st = AIBinder_transact(g_svc, TX_UPDATE_HEADSET_POSE_FIELD, &in, &out, 0);
  if (st != STATUS_OK) { fprintf(stderr, "[-] transact: %d\n", st); return -1; }
  // Reply is binder::Status then the bool. A non-zero exception code means the call was rejected
  // (this is how "running as shell instead of root" shows up) -- report it rather than counting the
  // transaction as a delivered pose.
  int32_t exception = 0, accepted = 0;
  AParcel_readInt32(out, &exception);
  AParcel_readInt32(out, &accepted);
  AParcel_delete(out);
  if (exception != 0) { fprintf(stderr, "[-] service returned exception %d\n", exception); return -1; }
  return accepted ? 0 : 1;   // 1 = service said "false"
}

static int inject_pose(const float p[3], const float q_xyzw[4]) {
  int a = inject_field(FIELD_POSITION, p, 3);
  int b = inject_field(FIELD_ORIENTATION, q_xyzw, 4);
  return (a == 0 && b == 0) ? 0 : -1;
}

struct pose { double t; float p[3]; float q[4]; };

// TUM format: t x y z qx qy qz qw  (what euroc_runner writes)
static int load_traj(const char *path, struct pose **out) {
  FILE *f = fopen(path, "r");
  if (!f) { perror(path); return -1; }
  int cap = 4096, n = 0;
  struct pose *v = malloc((size_t)cap * sizeof *v);
  char line[512];
  while (fgets(line, sizeof line, f)) {
    if (line[0] == '#') continue;
    double t, x, y, z, qx, qy, qz, qw;
    if (sscanf(line, "%lf %lf %lf %lf %lf %lf %lf %lf", &t, &x, &y, &z, &qx, &qy, &qz, &qw) != 8)
      continue;
    if (n == cap) { cap *= 2; v = realloc(v, (size_t)cap * sizeof *v); }
    v[n].t = t;
    v[n].p[0] = (float)x; v[n].p[1] = (float)y; v[n].p[2] = (float)z;
    v[n].q[0] = (float)qx; v[n].q[1] = (float)qy; v[n].q[2] = (float)qz; v[n].q[3] = (float)qw;
    n++;
  }
  fclose(f);
  *out = v;
  return n;
}

int main(int argc, char **argv) {
  if (argc < 2) {
    fprintf(stderr, "usage: %s <traj.txt> [hz]\n       %s --circle [hz] [seconds]\n", argv[0], argv[0]);
    return 2;
  }
  signal(SIGINT, on_sig); signal(SIGTERM, on_sig);

  if (bind_service_manager() < 0) return 1;
  g_svc = AServiceManager_getService(SVC);
  if (!g_svc) { fprintf(stderr, "[-] service '%s' not found\n", SVC); return 1; }
  AIBinder_Class *clazz = AIBinder_Class_define(IFACE, cls_on_create, cls_on_destroy, cls_on_transact);
  if (!AIBinder_associateClass(g_svc, clazz)) {
    fprintf(stderr, "[-] associateClass failed (descriptor mismatch?)\n");
    return 1;
  }
  printf("[+] bound %s (%s)\n", SVC, IFACE);

  double hz = argc > 2 ? atof(argv[2]) : 30.0;
  if (hz <= 0) hz = 30.0;
  const double period = 1.0 / hz;

  struct pose *traj = NULL;
  int n = 0, circle = !strcmp(argv[1], "--circle");
  double circle_secs = argc > 3 ? atof(argv[3]) : 20.0;
  if (!circle) {
    n = load_traj(argv[1], &traj);
    if (n <= 0) { fprintf(stderr, "[-] no poses in %s\n", argv[1]); return 1; }
    printf("[+] loaded %d poses, replaying at %.1f Hz (%.1f s)\n", n, hz, n / hz);
  } else {
    printf("[+] synthetic circle at %.1f Hz for %.1f s\n", hz, circle_secs);
  }

  double t0 = now_s(), next = t0;
  long sent = 0, failed = 0, late = 0;
  double worst = 0;

  for (long i = 0; !g_stop; i++) {
    if (!circle && i >= n) break;
    if (circle && now_s() - t0 >= circle_secs) break;

    float p[3], q[4];
    if (circle) {
      double a = (now_s() - t0) * 0.8;
      p[0] = (float)(0.5 * cos(a)); p[1] = 0.0f; p[2] = (float)(0.5 * sin(a));
      // yaw about Y by -a, as xyzw
      q[0] = 0.0f; q[1] = (float)sin(-a / 2); q[2] = 0.0f; q[3] = (float)cos(-a / 2);
    } else {
      memcpy(p, traj[i].p, sizeof p);
      memcpy(q, traj[i].q, sizeof q);
    }

    if (inject_pose(p, q) == 0) sent++; else failed++;

    next += period;
    double slack = next - now_s();
    if (slack > 0) usleep((useconds_t)(slack * 1e6));
    else { late++; if (-slack > worst) worst = -slack; next = now_s(); }
  }

  double dur = now_s() - t0;
  printf("\n[+] sent %ld poses in %.2f s (%.1f Hz achieved), %ld failed\n",
         sent, dur, sent / dur, failed);
  printf("[%c] missed deadline on %ld frames (%.1f%%), worst overrun %.1f ms\n",
         late * 20 > sent ? '-' : '+', late, 100.0 * late / (sent + failed ? sent + failed : 1),
         worst * 1e3);
  AIBinder_decStrong(g_svc);
  return failed ? 1 : 0;
}
