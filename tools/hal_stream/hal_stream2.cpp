// hal_stream2.cpp — IMU FMQ streaming with a real hidl-gen-generated ISensorClient.
//
// The generated ISensorClient (from our reconstructed .hal) gives us a proper HIDL server
// object the HAL can call getSensorClientInfo() back on. IImu itself we still reach via
// asm()-bound vendor proxy symbols (we only generated ISensorClient). Same package/interface
// name => the generated ISensorClient IS vendor::oculus::hardware::sensors::V1_0::ISensorClient.

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <unistd.h>

#include <fmq/MessageQueue.h>
#include <hidl/MQDescriptor.h>
#include <hidl/HidlSupport.h>
#include <hidl/HidlTransportSupport.h>
#include <cutils/ashmem.h>
#include <cutils/native_handle.h>

#include <vendor/oculus/hardware/sensors/1.0/ISensorClient.h>

using android::hardware::MessageQueue;
using android::hardware::MQDescriptorSync;
using android::hardware::kSynchronizedReadWrite;
using android::hardware::hidl_handle;
using android::hardware::Return;
using android::hardware::Void;
namespace V1_0 = ::vendor::oculus::hardware::sensors::V1_0;

// sizeof(ImuData) = 64 (from client wrapper's MessageQueue<ImuData> ctor: numElements<<6).
struct ImuElem { uint8_t raw[64]; };
// FmqConfig: hidl_handle (EventFlag, empty) + 2x uint32 (24 bytes total).
struct FmqConfig { hidl_handle eventFlag; uint32_t a; uint32_t b; };

// ---- our ISensorClient server impl: trivial, returns a zeroed SensorClientInfo -------------
static volatile int g_infoCalls = 0;
struct MyClient : public V1_0::ISensorClient {
    Return<void> getSensorClientInfo(getSensorClientInfo_cb cb) override {
        g_infoCalls++;
        fprintf(stderr, "[getSensorClientInfo called #%d]\n", g_infoCalls);
        V1_0::SensorClientInfo info;
        info.name = "openvr";
        info.field0 = 0; info.field1 = 0; info.field2 = 0;
        cb(V1_0::Result::OK, info);
        return Void();
    }
};

// ---- asm()-bound vendor IImu proxy (we didn't generate IImu) --------------------------------
// NB: these HIDL methods return android::hardware::Return<void> (sret via x8), NOT an int.
struct SpHolder { void* p; ~SpHolder() {} };
using android::hardware::Return;
extern "C++" {
SpHolder IImu_getService(const std::string&, bool)
    asm("_ZN6vendor6oculus8hardware7sensors4V1_04IImu10getServiceERKNSt3__1"
        "12basic_stringIcNS5_11char_traitsIcEENS5_9allocatorIcEEEEb");
// prepareStream(this, MQDescriptor<ImuData,sync>&, sp<ISensorClient>&, FmqConfig&)
Return<void> BpHwImu_prepareStream(void* thiz, const MQDescriptorSync<ImuElem>&,
                             const android::sp<V1_0::ISensorClient>&, const FmqConfig&)
    asm("_ZN6vendor6oculus8hardware7sensors4V1_07BpHwImu13prepareStreamERKN7android8hardware"
        "12MQDescriptorINS3_7ImuDataELNS6_8MQFlavorE1EEERKNS5_2spINS3_13ISensorClientEEERKNS3_9FmqConfigE");
// streamControl(this, sp<ISensorClient>&, StreamCommand)
Return<void> BpHwImu_streamControl(void* thiz, const android::sp<V1_0::ISensorClient>&, uint32_t)
    asm("_ZN6vendor6oculus8hardware7sensors4V1_07BpHwImu13streamControlERKN7android2spINS3_"
        "13ISensorClientEEENS3_13StreamCommandE");
}

static void hexdump(const void* p, size_t n) {
  const uint8_t* b = (const uint8_t*)p;
  for (size_t i = 0; i < n; i += 16) {
    printf("%04zx  ", i);
    for (size_t j = 0; j < 16; j++) { if (i+j<n) printf("%02x ", b[i+j]); else printf("   "); if(j==7)printf(" "); }
    printf(" |"); for (size_t j=0;j<16&&i+j<n;j++){uint8_t c=b[i+j];putchar(c>=32&&c<127?c:'.');} printf("|\n");
  }
}

int main(int argc, char** argv) {
  setvbuf(stdout, nullptr, _IONBF, 0);
  uint32_t startCmd = argc > 1 ? (uint32_t)atoi(argv[1]) : 1;
  printf("== hal_stream2 (START cmd=%u) ==\n", startCmd);

  android::hardware::configureRpcThreadpool(4, false /*callerWillJoin*/);
  // actively serve incoming binder callbacks (getSensorClientInfo) on a dedicated thread
  std::thread([]{ android::hardware::joinRpcThreadpool(); }).detach();

  SpHolder imu = IImu_getService(std::string("default"), false);
  printf("IImu::getService -> %p\n", imu.p);
  if (!imu.p) return 1;

  MessageQueue<ImuElem, kSynchronizedReadWrite> mq(256, false);
  printf("mq.isValid=%d elem=%zu\n", mq.isValid(), sizeof(ImuElem));
  if (!mq.isValid()) return 2;

  android::sp<V1_0::ISensorClient> client = new MyClient();
  printf("client=%p\n", client.get());
  {
    auto b = ::android::hardware::toBinder<V1_0::ISensorClient>(client);
    printf("toBinder -> %p\n", b.get());
  }
  // Build a real EventFlag handle (ashmem fd wrapped in a hidl_handle) — the HAL creates an
  // EventFlag from FmqConfig.eventFlag and would crash on an empty handle.
  FmqConfig cfg; memset(&cfg, 0, sizeof(cfg));
  int efd = ashmem_create_region("openvr-imu-evflag", 4096);
  printf("eventflag ashmem fd=%d\n", efd);
  native_handle_t* nh = native_handle_create(1 /*fds*/, 0 /*ints*/);
  nh->data[0] = efd;
  cfg.eventFlag.setTo(nh, false /*shouldOwn*/);
  cfg.a = 0x8000; cfg.b = 0x1;   // RE-derived eventflag bits (1<<15, 1<<0)

  auto r1 = BpHwImu_prepareStream(imu.p, *mq.getDesc(), client, cfg);
  printf("prepareStream isOk=%d desc='%s'\n", r1.isOk(), r1.isOk() ? "" : r1.description().c_str());
  if (startCmd != 99) {
    auto r2 = BpHwImu_streamControl(imu.p, client, startCmd);
    printf("streamControl(START=%u) isOk=%d\n", startCmd, r2.isOk());
  } else {
    printf("(skipping streamControl — prepareStream-only test)\n");
  }

  ImuElem rec; int got = 0;
  for (int i = 0; i < 5000 && got < 6; i++) {
    size_t avail = mq.availableToRead();
    if (i % 500 == 0) printf("[t=%dms] availableToRead=%zu infoCalls=%d\n", i, avail, g_infoCalls);
    while (mq.read(&rec, 1)) {
      printf("\n[rec %d] availableToRead=%zu\n", got, mq.availableToRead());
      hexdump(&rec, 64);
      if (++got >= 6) break;
    }
    usleep(1000);
  }
  printf("\n== got %d records, infoCalls=%d ==\n", got, g_infoCalls);
  // stop cleanly, then bail past any crashy teardown
  (void)BpHwImu_streamControl(imu.p, client, startCmd == 1 ? 2 : 1);
  fflush(stdout); _exit(0);
}
