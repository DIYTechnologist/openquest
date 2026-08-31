// hal_stream.cpp — attempt IMU FMQ streaming from vendor.oculus.hardware.sensors@1.0::IImu.
//
// Uses real AOSP libfmq/libhidl headers (tools/aosp-headers/inc) built with the __1 ABI
// override, plus asm()-bound vendor proxy symbols. First experiment: pass a NULL ISensorClient
// and a minimal FmqConfig to prepareStream, to learn whether a full HIDL server object is
// actually required before data will flow.
//
// Element size (FMQ quantum) is a generous stand-in; real sizeof(ImuData) is recovered from
// the raw bytes once records arrive.

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <unistd.h>

#include <fmq/MessageQueue.h>
#include <hidl/MQDescriptor.h>
#include <hidl/HidlSupport.h>

using android::hardware::MessageQueue;
using android::hardware::MQDescriptorSync;
using android::hardware::kSynchronizedReadWrite;
using android::hardware::hidl_handle;

// Generous fixed element; first sizeof(ImuData) bytes will be the real record.
struct ImuElem { uint8_t raw[128]; };

// FmqConfig: 24 bytes = hidl_handle (16, holds EventFlag fd; empty here) + 2x uint32 scalars.
struct FmqConfig { hidl_handle eventFlag; uint32_t a; uint32_t b; };

enum class Result : int32_t {};
// sp<T>& is ABI-identical to a pointer to a single-pointer object; model interfaces opaquely.
struct SpHolder { void* p; ~SpHolder() {} };   // non-trivial dtor => sret, leaks (short-lived)

extern "C++" {
SpHolder IImu_getService(const std::string&, bool)
    asm("_ZN6vendor6oculus8hardware7sensors4V1_04IImu10getServiceERKNSt3__1"
        "12basic_stringIcNS5_11char_traitsIcEENS5_9allocatorIcEEEEb");

// BpHwImu::prepareStream(const MQDescriptor<ImuData,sync>&, const sp<ISensorClient>&, const FmqConfig&)
// client passed as pointer-to-(single-pointer sp storage).
Result BpHwImu_prepareStream(void* thiz, const MQDescriptorSync<ImuElem>&,
                             void* const* clientSp, const FmqConfig&)
    asm("_ZN6vendor6oculus8hardware7sensors4V1_07BpHwImu13prepareStreamERKN7android8hardware"
        "12MQDescriptorINS3_7ImuDataELNS6_8MQFlavorE1EEERKNS5_2spINS3_13ISensorClientEEERKNS3_9FmqConfigE");

// BpHwImu::streamControl(const sp<ISensorClient>&, StreamCommand=uint32)
Result BpHwImu_streamControl(void* thiz, void* const* clientSp, uint32_t)
    asm("_ZN6vendor6oculus8hardware7sensors4V1_07BpHwImu13streamControlERKN7android2spINS3_"
        "13ISensorClientEEENS3_13StreamCommandE");
}

static void hexdump(const void* p, size_t n) {
  const uint8_t* b = (const uint8_t*)p;
  for (size_t i = 0; i < n; i += 16) {
    printf("%04zx  ", i);
    for (size_t j = 0; j < 16; j++) { if (i + j < n) printf("%02x ", b[i+j]); else printf("   "); if (j==7) printf(" "); }
    printf(" |");
    for (size_t j = 0; j < 16 && i+j < n; j++) { uint8_t c=b[i+j]; putchar(c>=32&&c<127?c:'.'); }
    printf("|\n");
  }
}

int main(int argc, char** argv) {
  setvbuf(stdout, nullptr, _IONBF, 0);
  uint32_t cmd = argc > 1 ? (uint32_t)atoi(argv[1]) : 1;   // StreamCommand to try as START
  printf("== hal_stream (START cmd=%u) ==\n", cmd);

  SpHolder imu = IImu_getService(std::string("default"), false);
  printf("IImu::getService -> %p\n", imu.p);
  if (!imu.p) return 1;

  // Client-side FMQ: 256 elements, no internal EventFlag (poll).
  MessageQueue<ImuElem, kSynchronizedReadWrite> mq(256, false);
  printf("mq.isValid=%d quantum=%zu\n", mq.isValid(), sizeof(ImuElem));
  if (!mq.isValid()) return 2;

  FmqConfig cfg; memset(&cfg, 0, sizeof(cfg));   // empty eventflag handle, zero scalars
  void* clientNull = nullptr;                     // NULL ISensorClient — the experiment

  Result r1 = BpHwImu_prepareStream(imu.p, *mq.getDesc(), &clientNull, cfg);
  printf("prepareStream Result=%d\n", (int)r1);

  Result r2 = BpHwImu_streamControl(imu.p, &clientNull, cmd);
  printf("streamControl(START=%u) Result=%d\n", cmd, (int)r2);

  // Poll the FMQ for records.
  ImuElem rec;
  int got = 0;
  for (int i = 0; i < 2000 && got < 4; i++) {   // ~2s at 1ms
    while (mq.read(&rec, 1)) {
      printf("\n[record %d] availableToRead=%zu\n", got, mq.availableToRead());
      hexdump(&rec, 128);
      if (++got >= 4) break;
    }
    usleep(1000);
  }
  printf("\n== got %d records ==\n", got);
  _exit(0);
}
