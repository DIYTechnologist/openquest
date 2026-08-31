// hal_side_shim.cpp — LD_PRELOAD into the HAL service (vendor.oculus...sensors@1.0-service).
// Observes the HAL's OUTGOING callbacks to clients: BpHwSensorClient::getSensorClientInfo.
// If the HAL tries to query our client and fails, we see the attempt here even though our
// client's infoCalls stays 0. Logs to a distinct file, forwards to the real impl.

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdarg>
#include <dlfcn.h>
#include <fcntl.h>
#include <unistd.h>

#include <hidl/HidlSupport.h>
using android::hardware::Return;

static void logf(const char* fmt, ...) {
  char buf[512]; va_list ap; va_start(ap, fmt); vsnprintf(buf, sizeof(buf), fmt, ap); va_end(ap);
  int fd = open("/data/local/tmp/hal_side.log", O_WRONLY|O_CREAT|O_APPEND, 0666);
  if (fd >= 0) { write(fd, buf, strlen(buf)); close(fd); }
}

// BpHwSensorClient::getSensorClientInfo(std::function<void(Result, const SensorClientInfo&)>)
// non-static member: (this, function<...>). std::function passed by hidden pointer.
// Returns Return<void> (sret via x8 -> first hidden arg).
#define SCINFO_MANGLED \
  "_ZN6vendor6oculus8hardware7sensors4V1_016BpHwSensorClient19getSensorClientInfo" \
  "ENSt3__18functionIFvNS3_6ResultERKNS3_16SensorClientInfoEEEE"

// Return<void> is returned by value (sret). Model as: the compiler passes a hidden 1st ptr for
// the return object, then this, then the function ptr. Use a struct-return-compatible thunk.
struct RetVoid { char storage[16]; };  // Return<void> ~ {mCheckedOnDestruction:bool, status/desc}

extern "C++" Return<void> scinfo_out(void* thiz, void* fn) asm(SCINFO_MANGLED);
Return<void> scinfo_out(void* thiz, void* fn) {
  logf("==== HAL -> BpHwSensorClient::getSensorClientInfo (proxy this=%p) ====\n", thiz);
  using Fn = Return<void>(*)(void*, void*);
  static Fn real = (Fn)dlsym(RTLD_NEXT, SCINFO_MANGLED);
  Return<void> r = real(thiz, fn);
  logf("     returned isOk=%d\n", r.isOk());
  return r;
}

// SERVER dispatch (in the HAL): BnHwImu::_hidl_prepareStream / _hidl_streamControl.
// static (BnHwBase* this, const Parcel& in, Parcel* out, function<void(Parcel&)> cb) -> status_t(int)
#define SRV_PREP \
  "_ZN6vendor6oculus8hardware7sensors4V1_07BnHwImu17_hidl_prepareStream" \
  "EPNS_4hidl4base4V1_08BnHwBaseERKN7android8hardware6ParcelEPSC_NSt3__18functionIFvRSC_EEE"
#define SRV_STREAM \
  "_ZN6vendor6oculus8hardware7sensors4V1_07BnHwImu17_hidl_streamControl" \
  "EPNS_4hidl4base4V1_08BnHwBaseERKN7android8hardware6ParcelEPSC_NSt3__18functionIFvRSC_EEE"

extern "C++" int srv_prep(void* self, const void* in, void* out, void* cb) asm(SRV_PREP);
int srv_prep(void* self, const void* in, void* out, void* cb) {
  logf("==== HAL server BnHwImu::_hidl_prepareStream (self=%p) ====\n", self);
  using Fn = int(*)(void*, const void*, void*, void*);
  static Fn real = (Fn)dlsym(RTLD_NEXT, SRV_PREP);
  int rc = real(self, in, out, cb);
  logf("     _hidl_prepareStream rc=%d\n", rc);
  return rc;
}

extern "C++" int srv_stream(void* self, const void* in, void* out, void* cb) asm(SRV_STREAM);
int srv_stream(void* self, const void* in, void* out, void* cb) {
  logf("==== HAL server BnHwImu::_hidl_streamControl (self=%p) ====\n", self);
  using Fn = int(*)(void*, const void*, void*, void*);
  static Fn real = (Fn)dlsym(RTLD_NEXT, SRV_STREAM);
  int rc = real(self, in, out, cb);
  logf("     _hidl_streamControl rc=%d\n", rc);
  return rc;
}
