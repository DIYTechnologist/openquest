// remote_pose_probe.c — inject a distinctive, known controller pose via updateRemotePoseField,
// so its bit pattern can be searched for in TrackingServiceController's shared memory.
//
// Same purpose as research-notes/28's head-tracker discovery (inject a known value, then search
// for it with mempeek) but for the controller region, which was only ever noted as a candidate
// location (research-notes/19, research-notes/24) and never actually probed.
//
// Interface: oculus.internal.virtual_input.ITrackingDataInjectionService, transaction code 1,
// updateRemotePoseField(String16 id, int32 field, float[] values, bool*) -- code and field map
// (0=position xyz, 1=orientation quat xyzw) both recovered by disassembly in research-notes/23.
// Same STABLE NDK binder API as components/tracking/src/pose_inject.c; not merged into that file
// since this is throwaway bring-up code for one controller-region discovery step, not a lasting
// part of the tracking component.
//
// Usage: remote_pose_probe <controller_id_hex> <x> <y> <z>
//        remote_pose_probe <controller_id_hex> --circle [hz] [seconds]

#define _GNU_SOURCE
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <android/binder_ibinder.h>
#include <android/binder_parcel.h>
#include <android/binder_status.h>
#include <dlfcn.h>

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
#define TX_UPDATE_REMOTE_POSE_FIELD 1
#define FIELD_POSITION    0
#define FIELD_ORIENTATION 1

static void *cls_on_create(void *args) { return args; }
static void cls_on_destroy(void *ud) { (void)ud; }
static binder_status_t cls_on_transact(AIBinder *b, transaction_code_t c, const AParcel *in,
                                       AParcel *out) {
  (void)b; (void)c; (void)in; (void)out; return STATUS_UNKNOWN_TRANSACTION;
}

static AIBinder *g_svc = NULL;
static const char *g_id = NULL;

static double now_s(void) {
  struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec + ts.tv_nsec * 1e-9;
}

static int inject_remote_field(int field, const float *v, int n) {
  AParcel *in = NULL, *out = NULL;
  binder_status_t st = AIBinder_prepareTransaction(g_svc, &in);
  if (st != STATUS_OK) { fprintf(stderr, "[-] prepareTransaction: %d\n", st); return -1; }
  if (AParcel_writeString(in, g_id, (int32_t)strlen(g_id)) != STATUS_OK) return -1;
  if (AParcel_writeInt32(in, field) != STATUS_OK) return -1;
  if (AParcel_writeFloatArray(in, v, n) != STATUS_OK) return -1;
  st = AIBinder_transact(g_svc, TX_UPDATE_REMOTE_POSE_FIELD, &in, &out, 0);
  if (st != STATUS_OK) { fprintf(stderr, "[-] transact: %d\n", st); return -1; }
  int32_t exception = 0, accepted = 0;
  AParcel_readInt32(out, &exception);
  AParcel_readInt32(out, &accepted);
  AParcel_delete(out);
  if (exception != 0) { fprintf(stderr, "[-] service returned exception %d\n", exception); return -1; }
  return accepted ? 0 : 1;
}

int main(int argc, char **argv) {
  if (argc < 3) {
    fprintf(stderr, "usage: %s <controller_id_hex> <x> <y> <z>\n"
                    "       %s <controller_id_hex> --circle [hz] [seconds]\n", argv[0], argv[0]);
    return 2;
  }
  g_id = argv[1];

  if (bind_service_manager() < 0) return 1;
  g_svc = AServiceManager_getService(SVC);
  if (!g_svc) { fprintf(stderr, "[-] service '%s' not found\n", SVC); return 1; }
  AIBinder_Class *clazz = AIBinder_Class_define(IFACE, cls_on_create, cls_on_destroy, cls_on_transact);
  if (!AIBinder_associateClass(g_svc, clazz)) {
    fprintf(stderr, "[-] associateClass failed (descriptor mismatch?)\n");
    return 1;
  }
  printf("[+] bound %s (%s), remote id=%s\n", SVC, IFACE, g_id);

  if (!strcmp(argv[2], "--circle")) {
    double hz = argc > 3 ? atof(argv[3]) : 10.0;
    double secs = argc > 4 ? atof(argv[4]) : 20.0;
    if (hz <= 0) hz = 10.0;
    printf("[+] injecting synthetic circle radius 0.5 at %.1f Hz for %.1f s\n", hz, secs);
    double t0 = now_s(), next = t0;
    long sent = 0, failed = 0;
    while (now_s() - t0 < secs) {
      double a = (now_s() - t0) * 0.8;
      float p[3] = { (float)(0.5 * cos(a)), 0.0f, (float)(0.5 * sin(a)) };
      float q[4] = { 0.0f, (float)sin(-a / 2), 0.0f, (float)cos(-a / 2) };
      int ra = inject_remote_field(FIELD_POSITION, p, 3);
      int rb = inject_remote_field(FIELD_ORIENTATION, q, 4);
      if (ra == 0 && rb == 0) sent++; else failed++;
      next += 1.0 / hz;
      double slack = next - now_s();
      if (slack > 0) usleep((useconds_t)(slack * 1e6)); else next = now_s();
    }
    printf("[+] sent %ld, failed %ld\n", sent, failed);
  } else {
    if (argc < 5) { fprintf(stderr, "need x y z\n"); return 2; }
    float p[3] = { (float)atof(argv[2]), (float)atof(argv[3]), (float)atof(argv[4]) };
    int rc = inject_remote_field(FIELD_POSITION, p, 3);
    printf("[%c] updateRemotePoseField(%s, POSITION, {%.6f, %.6f, %.6f}) -> rc=%d\n",
           rc == 0 ? '+' : '-', g_id, p[0], p[1], p[2], rc);
  }

  AIBinder_decStrong(g_svc);
  return 0;
}
