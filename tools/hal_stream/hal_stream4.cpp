// hal_stream4.cpp — like hal_stream3 but with the CANONICAL hwbinder serving pattern:
// the MAIN thread joins the RPC threadpool (so the HAL's async callbacks into our client —
// getSensorClientInfo — are actually served), and a worker thread runs the protocol + reads.
//
// Hypothesis under test: hal_stream3 got infoCalls=0 because its detached joinRpcThreadpool
// thread wasn't reliably serving the HAL's callback, so the HAL dropped our client and never
// streamed. Here we serve on main, which is how real HAL clients (trackingservice) are built.

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <atomic>
#include <unistd.h>

#include <fmq/MessageQueue.h>
#include <hidl/HidlTransportSupport.h>
#include <cutils/ashmem.h>
#include <sys/mman.h>
#include <cutils/native_handle.h>

#include <vendor/oculus/hardware/sensors/1.0/IImu.h>
#include <vendor/oculus/hardware/sensors/1.0/ISensorClient.h>

using android::sp;
using android::hardware::MessageQueue;
using android::hardware::kSynchronizedReadWrite;
using android::hardware::Return;
using android::hardware::Void;
namespace V1_0 = ::vendor::oculus::hardware::sensors::V1_0;
using ImuMQ = MessageQueue<V1_0::ImuData, kSynchronizedReadWrite>;

static std::atomic<int> g_infoCalls{0};
struct MyClient : public V1_0::ISensorClient {
    Return<void> getSensorClientInfo(getSensorClientInfo_cb cb) override {
        int n = ++g_infoCalls;
        fprintf(stderr, "[getSensorClientInfo CALLED #%d]\n", n);
        V1_0::SensorClientInfo info; info.name = "openvr";
        info.field0 = 0; info.field1 = 0; info.field2 = 0;
        cb(V1_0::Result::OK, info);
        return Void();
    }
};

static void hexdump(const void* p, size_t n) {
  const uint8_t* b = (const uint8_t*)p;
  for (size_t i = 0; i < n; i += 16) {
    printf("%04zx  ", i);
    for (size_t j = 0; j < 16; j++) { if (i+j<n) printf("%02x ", b[i+j]); else printf("   "); if(j==7)printf(" "); }
    printf("\n");
  }
}

static uint32_t g_cmd = 0;

static void protocol_thread() {
  sp<V1_0::IImu> imu = V1_0::IImu::getService("default", false);
  printf("IImu::getService -> %p\n", imu.get());
  if (!imu) { _exit(1); }

  imu->getProperties([](V1_0::Result r, const V1_0::MotionSensorProperties& p){
    printf("getProperties Result=%d sensor='%s' label='%s' rate=%g\n",
           (int)r, p.sensorType.c_str(), p.label.c_str(), p.rate);
  });

  ImuMQ mq(256, false);
  printf("mq.isValid=%d elem=%zu\n", mq.isValid(), sizeof(V1_0::ImuData));
  if (!mq.isValid()) { _exit(2); }

  sp<V1_0::ISensorClient> client = new MyClient();

  int efd = ashmem_create_region("openvr-imu-evflag", 4);
  ashmem_set_prot_region(efd, PROT_READ | PROT_WRITE);
  native_handle_t* nh = native_handle_create(1, 0); nh->data[0] = efd;
  V1_0::FmqConfig cfg;
  cfg.eventFlag.setTo(nh, true);
  cfg.bitA = 0x8000; cfg.bitB = 0x1;

  auto r1 = imu->prepareStream(*mq.getDesc(), client, cfg);
  printf("prepareStream isOk=%d\n", r1.isOk());
  auto r2 = imu->streamControl(client, (V1_0::StreamCommand)g_cmd);
  printf("streamControl(%u) isOk=%d\n", g_cmd, r2.isOk());

  V1_0::ImuData rec; int got = 0;
  for (int i = 0; i < 120000 && got < 6; i++) {
    if (i % 500 == 0) printf("[t=%dms] availableToRead=%zu infoCalls=%d\n",
                              i, mq.availableToRead(), g_infoCalls.load());
    while (mq.read(&rec, 1)) {
      printf("\n[rec %d]\n", got); hexdump(&rec, sizeof(rec));
      if (++got >= 6) break;
    }
    usleep(1000);
  }
  printf("\n== got %d records, infoCalls=%d ==\n", got, g_infoCalls.load());
  fflush(stdout); _exit(0);
}

int main(int argc, char** argv) {
  setvbuf(stdout, nullptr, _IONBF, 0);
  g_cmd = argc > 1 ? (uint32_t)atoi(argv[1]) : 0;
  printf("== hal_stream4 (main-thread serving, cmd=%u) ==\n", g_cmd);

  android::hardware::configureRpcThreadpool(4, true /*callerWillJoin*/);
  std::thread(protocol_thread).detach();
  android::hardware::joinRpcThreadpool();   // main serves callbacks forever
  return 0;
}
