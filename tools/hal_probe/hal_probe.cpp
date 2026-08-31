// hal_probe.cpp — live enumeration probe for vendor.oculus.hardware.sensors@1.0
//
// Goal: recover the on-wire byte layout of the HIDL POD structs (MotionSensorProperties,
// CameraProperties, ChannelSettings) empirically, by calling the HAL's getProperties/
// getChannels and hex-dumping the struct memory handed to the reply callback.
//
// Strategy: we have NO HIDL headers (NDK doesn't ship them). Instead we bind directly to the
// exported proxy symbols in the on-device vendor .so via asm() labels, so the compiler must
// only get the *call ABI* right, not the mangling. Callback structs arrive by const-reference
// (i.e. a pointer), so we can dump their bytes without knowing their real definition.
//
// Build: NDK aarch64 clang++, linked against pulled device libs. See build.sh.
// Run  : as root, SELinux permissive (needs hwservice_manager find). Read-only queries.

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <unistd.h>   // _exit

// ---- minimal ABI-compatible stand-ins (layout/size only need to match how we USE them) ----
namespace android {
template <class T>
struct sp {          // libutils sp<T>: single pointer member; non-trivial dtor => sret via x8
  T* p = nullptr;
  ~sp() {}           // no-op: we intentionally leak one strong ref (process is short-lived)
};
namespace hardware {
template <class T>
struct hidl_vec {    // real: T* mBuffer; uint32_t mSize; bool mOwnsBuffer;  (16 bytes)
  T* buffer;
  uint32_t size;
  bool owns;
};
}  // namespace hardware
}  // namespace android
using android::sp;
using android::hardware::hidl_vec;

enum class Result : int32_t {};        // HIDL enum, underlying int32
struct IImu;                            // opaque proxy types
struct ICameraProvider;
struct MotionSensorProperties;          // opaque POD; only ever used by const-ref (pointer)
struct CameraProperties;
struct ChannelSettings;

// ---- exported device symbols, bound by mangled name (compiler won't re-mangle) ----------
extern "C++" {
// IImu::getService(const std::string&, bool) -> sp<IImu>
sp<IImu> IImu_getService(const std::string&, bool)
    asm("_ZN6vendor6oculus8hardware7sensors4V1_04IImu10getServiceERKNSt3__1"
        "12basic_stringIcNS5_11char_traitsIcEENS5_9allocatorIcEEEEb");

// ICameraProvider::getService(const std::string&, bool) -> sp<ICameraProvider>
sp<ICameraProvider> ICam_getService(const std::string&, bool)
    asm("_ZN6vendor6oculus8hardware7sensors4V1_015ICameraProvider10getServiceERKNSt3__1"
        "12basic_stringIcNS5_11char_traitsIcEENS5_9allocatorIcEEEEb");

// BpHwImu::getProperties(std::function<void(Result, const MotionSensorProperties&)>)  [member: this=x0]
void BpHwImu_getProperties(void* thiz,
                           std::function<void(Result, const MotionSensorProperties&)> cb)
    asm("_ZN6vendor6oculus8hardware7sensors4V1_07BpHwImu13getPropertiesENSt3__1"
        "8functionIFvNS3_6ResultERKNS3_22MotionSensorPropertiesEEEE");

// BpHwCameraProvider::getProperties(std::function<void(Result, const hidl_vec<CameraProperties>&)>)
void BpHwCam_getProperties(
    void* thiz,
    std::function<void(Result, const hidl_vec<CameraProperties>&)> cb)
    asm("_ZN6vendor6oculus8hardware7sensors4V1_018BpHwCameraProvider13getPropertiesENSt3__1"
        "8functionIFvNS3_6ResultERKN7android8hardware8hidl_vecINS3_16CameraPropertiesEEEEEE");

// BpHwCameraProvider::getChannels(std::function<void(const ChannelSettings&)>)
void BpHwCam_getChannels(void* thiz, std::function<void(const ChannelSettings&)> cb)
    asm("_ZN6vendor6oculus8hardware7sensors4V1_018BpHwCameraProvider11getChannelsENSt3__1"
        "8functionIFvRKNS3_15ChannelSettingsEEEE");
}

// hidl_string on-wire/in-memory: { const char* ptr; uint32_t size; bool owns; } (16 bytes)
struct HStr { const char* ptr; uint32_t size; uint8_t owns; uint8_t pad[3]; };
static void show_hstr(const char* tag, const HStr* s) {
  // print at most 80 chars of the referenced buffer
  char buf[81]; uint32_t n = s->size < 80 ? s->size : 80;
  if (s->ptr) { memcpy(buf, s->ptr, n); buf[n] = 0; } else buf[0] = 0;
  printf("  %s: HStr{ptr=%p size=%u owns=%u} = \"%s%s\"\n",
         tag, (void*)s->ptr, s->size, s->owns, buf, s->size > 80 ? "..." : "");
}

static void hexdump(const char* tag, const void* p, size_t n) {
  const uint8_t* b = reinterpret_cast<const uint8_t*>(p);
  printf("---- %s : %zu bytes @ %p ----\n", tag, n, p);
  for (size_t i = 0; i < n; i += 16) {
    printf("%04zx  ", i);
    for (size_t j = 0; j < 16; j++) {
      if (i + j < n) printf("%02x ", b[i + j]); else printf("   ");
      if (j == 7) printf(" ");
    }
    printf(" |");
    for (size_t j = 0; j < 16 && i + j < n; j++) {
      uint8_t c = b[i + j];
      putchar(c >= 32 && c < 127 ? c : '.');
    }
    printf("|\n");
  }
}

// The lib's post-callback cleanup trips over our type-erased std::function stand-in, so we
// grab the data and _exit(0) from inside each callback (before the crashy return path).
int main(int argc, char** argv) {
  setvbuf(stdout, nullptr, _IONBF, 0);
  const char* which = argc > 1 ? argv[1] : "imu";
  printf("== hal_probe [%s] ==\n", which);

  if (!strcmp(which, "imu")) {
    sp<IImu> imu = IImu_getService(std::string("default"), false);
    printf("IImu::getService -> %p\n", imu.p);
    if (!imu.p) return 1;
    BpHwImu_getProperties(imu.p, [](Result r, const MotionSensorProperties& mp) {
      printf("[IImu.getProperties] Result=%d\n", (int)r);
      const uint8_t* base = reinterpret_cast<const uint8_t*>(&mp);
      hexdump("MotionSensorProperties head 96B", base, 96);
      // hypothesis: 4x hidl_string then float rate
      for (int i = 0; i < 4; i++) {
        char tag[16]; snprintf(tag, sizeof(tag), "hstr[%d]", i);
        show_hstr(tag, reinterpret_cast<const HStr*>(base + i * 16));
      }
      printf("  float@0x40 = %g   float@0x44 = %g\n",
             *reinterpret_cast<const float*>(base + 0x40),
             *reinterpret_cast<const float*>(base + 0x44));
      fflush(stdout); _exit(0);
    });
  } else if (!strcmp(which, "cam")) {
    sp<ICameraProvider> cam = ICam_getService(std::string("default"), false);
    printf("ICameraProvider::getService -> %p\n", cam.p);
    if (!cam.p) return 1;
    BpHwCam_getProperties(cam.p, [](Result r, const hidl_vec<CameraProperties>& v) {
      printf("[getProperties] Result=%d count=%u buffer=%p\n", (int)r, v.size, (void*)v.buffer);
      if (v.buffer && v.size) {
        // dump generously; infer stride = (addr of elem1 - elem0) once we see repetition
        size_t span = (size_t)v.size * 320; if (span > 6144) span = 6144;
        hexdump("CameraProperties[] raw", v.buffer, span);
        printf("  candidate stride = %zu bytes (span/count)\n", span / v.size);
      }
      fflush(stdout); _exit(0);
    });
  } else if (!strcmp(which, "chan")) {
    sp<ICameraProvider> cam = ICam_getService(std::string("default"), false);
    printf("ICameraProvider::getService -> %p\n", cam.p);
    if (!cam.p) return 1;
    BpHwCam_getChannels(cam.p, [](const ChannelSettings& c) {
      hexdump("ChannelSettings head 128B", &c, 128);
      fflush(stdout); _exit(0);
    });
  }
  printf("== done (no callback fired) ==\n");
  return 0;
}
