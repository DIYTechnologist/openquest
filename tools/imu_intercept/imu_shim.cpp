// imu_shim.cpp — LD_PRELOAD interposer for BpHwImu::prepareStream, to capture the exact
// MQDescriptor + FmqConfig a *working* IImu client (trackingservice) sends. Dumps args, then
// forwards to the real vendor implementation via RTLD_NEXT.
//
// Build: NDK aarch64 clang++ (see build.sh). Load: LD_PRELOAD into trackingservice.

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdarg>
#include <dlfcn.h>
#include <fcntl.h>
#include <unistd.h>

#include <hidl/HidlSupport.h>
#include <hidl/MQDescriptor.h>
using android::hardware::Return;

// mangled name of vendor::oculus::hardware::sensors::V1_0::BpHwImu::prepareStream(
//   const MQDescriptor<ImuData,sync>&, const sp<ISensorClient>&, const FmqConfig&)
#define PREP_MANGLED \
  "_ZN6vendor6oculus8hardware7sensors4V1_07BpHwImu13prepareStreamERKN7android8hardware" \
  "12MQDescriptorINS3_7ImuDataELNS6_8MQFlavorE1EEERKNS5_2spINS3_13ISensorClientEEERKNS3_9FmqConfigE"

static void logf(const char* fmt, ...) {
  char buf[512]; va_list ap; va_start(ap, fmt); vsnprintf(buf, sizeof(buf), fmt, ap); va_end(ap);
  int fd = open("/data/local/tmp/imu_intercept.log", O_WRONLY|O_CREAT|O_APPEND, 0666);
  if (fd >= 0) { write(fd, buf, strlen(buf)); close(fd); }
}

static void dumphex(const char* tag, const void* p, size_t n) {
  const uint8_t* b = (const uint8_t*)p;
  char line[128]; int off = 0;
  logf("  %s (%zu B @ %p):\n", tag, n, p);
  for (size_t i = 0; i < n; i += 16) {
    off = snprintf(line, sizeof(line), "    %04zx  ", i);
    for (size_t j = 0; j < 16 && i+j < n; j++) off += snprintf(line+off, sizeof(line)-off, "%02x ", b[i+j]);
    off += snprintf(line+off, sizeof(line)-off, "\n");
    logf("%s", line);
  }
}

// MQDescriptor layout (android::hardware::MQDescriptor): hidl_vec<GrantorDescriptor> grantors(16)
// + native_handle* mHandle(8) + uint32 mQuantum(4) + uint32 mFlags(4). FmqConfig: 24 B.
static void dumpDesc(const void* mqdesc) {
  dumphex("MQDescriptor head", mqdesc, 32);
  const uint8_t* d = (const uint8_t*)mqdesc;
  const void* grantorBuf = *(void* const*)(d + 0);
  uint32_t grantorCount  = *(const uint32_t*)(d + 8);
  const native_handle_t* nh = *(const native_handle_t* const*)(d + 16);
  uint32_t quantum = *(const uint32_t*)(d + 24);
  uint32_t flags   = *(const uint32_t*)(d + 28);
  logf("  grantorCount=%u quantum=%u flags=%u handle=%p\n", grantorCount, quantum, flags, nh);
  if (grantorBuf && grantorCount) dumphex("grantors[]", grantorBuf, grantorCount * 16);
  if (nh) { logf("  native_handle numFds=%d numInts=%d\n", nh->numFds, nh->numInts);
            dumphex("native_handle data", nh->data, (nh->numFds + nh->numInts) * 4); }
}
static void dumpFmqConfig(const void* cfg) {
  dumphex("FmqConfig", cfg, 24);
  const uint8_t* c = (const uint8_t*)cfg;
  const native_handle_t* nh = *(const native_handle_t* const*)(c + 0);  // hidl_handle.mHandle
  uint32_t bitA = *(const uint32_t*)(c + 16);
  uint32_t bitB = *(const uint32_t*)(c + 20);
  logf("  eventFlag handle=%p bitA=0x%x bitB=0x%x\n", nh, bitA, bitB);
  if (nh) { logf("  ef native_handle numFds=%d numInts=%d\n", nh->numFds, nh->numInts);
            dumphex("ef data", nh->data, (nh->numFds + nh->numInts) * 4); }
}

extern "C++" Return<void> imu_prepareStream(void* thiz, const void* mqdesc,
                                            const void* client, const void* fmqcfg) asm(PREP_MANGLED);

Return<void> imu_prepareStream(void* thiz, const void* mqdesc, const void* client, const void* fmqcfg) {
  logf("\n==== BpHwImu::prepareStream intercepted (this=%p client=%p) ====\n", thiz, client);
  dumpDesc(mqdesc);
  dumpFmqConfig(fmqcfg);
  using Fn = Return<void>(*)(void*, const void*, const void*, const void*);
  static Fn real = (Fn)dlsym(RTLD_NEXT, PREP_MANGLED);
  logf("  forwarding to real=%p\n", (void*)real);
  return real(thiz, mqdesc, client, fmqcfg);
}

// streamControl(this, const sp<ISensorClient>&, StreamCommand)  — StreamCommand is int32 by value
#define STREAM_MANGLED \
  "_ZN6vendor6oculus8hardware7sensors4V1_07BpHwImu13streamControlERKN7android2spINS3_" \
  "13ISensorClientEEENS3_13StreamCommandE"

extern "C++" Return<void> imu_streamControl(void* thiz, const void* client, int32_t cmd) asm(STREAM_MANGLED);

Return<void> imu_streamControl(void* thiz, const void* client, int32_t cmd) {
  logf("\n==== BpHwImu::streamControl intercepted (this=%p client=%p cmd=%d) ====\n", thiz, client, cmd);
  using Fn = Return<void>(*)(void*, const void*, int32_t);
  static Fn real = (Fn)dlsym(RTLD_NEXT, STREAM_MANGLED);
  return real(thiz, client, cmd);
}

// SERVER side in trackingservice: does the HAL ever call back into our getSensorClientInfo?
// BnHwSensorClient::_hidl_getSensorClientInfo(BnHwBase*, const Parcel&, Parcel*, function<void(Parcel&)>)
// static dispatch, returns status_t(int). std::function passed by hidden pointer.
#define SCINFO_MANGLED \
  "_ZN6vendor6oculus8hardware7sensors4V1_016BnHwSensorClient25_hidl_getSensorClientInfo" \
  "EPN7android4hidl4base4V1_08BnHwBaseERKNS5_8hardware6ParcelEPSC_NSt3__18functionIFvRSC_EEE"

extern "C++" int scinfo_dispatch(void* self, const void* p_in, void* p_out, void* cb) asm(SCINFO_MANGLED);
int scinfo_dispatch(void* self, const void* p_in, void* p_out, void* cb) {
  logf("\n==== BnHwSensorClient::getSensorClientInfo CALLED BY HAL (self=%p) ====\n", self);
  using Fn = int(*)(void*, const void*, void*, void*);
  static Fn real = (Fn)dlsym(RTLD_NEXT, SCINFO_MANGLED);
  return real(self, p_in, p_out, cb);
}
