// hal_stream5.cpp — participate in the FMQ EventFlag handshake, which hal_stream3/4's
// non-blocking poll never did. Hypothesis: the HAL's writer only writes after the reader
// signals "space available" (bitB=0x1) on the shared EventFlag; a pure poll never wakes it.
//
// bit map (from FmqConfig bitA=0x8000, bitB=0x1; FMQ_NOT_FULL=0x01):
//   0x8000 = data-available  (HAL writer WAKES it, we WAIT on it)
//   0x0001 = space-available (we WAKE it after reading, HAL writer WAITS on it)
// We build our own EventFlag from the SAME ashmem fd we hand the HAL, proactively wake the
// space bit once, then readBlocking.

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <atomic>
#include <unistd.h>

#include <fmq/MessageQueue.h>
#include <fmq/EventFlag.h>
#include <hidl/HidlTransportSupport.h>
#include <cutils/ashmem.h>
#include <sys/mman.h>
#include <cutils/native_handle.h>

#include <vendor/oculus/hardware/sensors/1.0/IImu.h>
#include <vendor/oculus/hardware/sensors/1.0/ISensorClient.h>

using android::sp;
using android::hardware::MessageQueue;
using android::hardware::kSynchronizedReadWrite;
using android::hardware::EventFlag;
using android::hardware::Return;
using android::hardware::Void;
namespace V1_0 = ::vendor::oculus::hardware::sensors::V1_0;
using ImuMQ = MessageQueue<V1_0::ImuData, kSynchronizedReadWrite>;

static const uint32_t BIT_DATA  = 0x8000;  // writer -> reader
static const uint32_t BIT_SPACE = 0x0001;  // reader -> writer

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
    printf("getProperties Result=%d sensor='%s' rate=%g\n", (int)r, p.sensorType.c_str(), p.rate);
  });

  ImuMQ mq(256, false);
  if (!mq.isValid()) { _exit(2); }
  sp<V1_0::ISensorClient> client = new MyClient();

  int efd = ashmem_create_region("openvr-imu-evflag", 4);
  ashmem_set_prot_region(efd, PROT_READ | PROT_WRITE);

  // our own EventFlag over the SAME ashmem region we give the HAL
  EventFlag* efGroup = nullptr;
  auto st = EventFlag::createEventFlag(efd, 0, &efGroup);
  printf("createEventFlag st=%d ef=%p\n", (int)st, efGroup);

  native_handle_t* nh = native_handle_create(1, 0); nh->data[0] = efd;
  V1_0::FmqConfig cfg;
  cfg.eventFlag.setTo(nh, true);
  cfg.bitA = BIT_DATA; cfg.bitB = BIT_SPACE;

  auto r1 = imu->prepareStream(*mq.getDesc(), client, cfg);
  printf("prepareStream isOk=%d\n", r1.isOk());
  auto r2 = imu->streamControl(client, (V1_0::StreamCommand)g_cmd);
  printf("streamControl(%u) isOk=%d\n", g_cmd, r2.isOk());

  // proactively tell the writer there is space
  if (efGroup) efGroup->wake(BIT_SPACE);

  V1_0::ImuData rec; int got = 0;
  for (int i = 0; i < 200 && got < 6; i++) {
    bool ok = mq.readBlocking(&rec, 1, BIT_SPACE /*we wake on read*/,
                              BIT_DATA /*we wait for data*/, 100000000LL /*100ms*/, efGroup);
    if (ok) {
      printf("\n[rec %d] (readBlocking) \n", got); hexdump(&rec, sizeof(rec));
      if (++got >= 6) break;
    } else if (i % 5 == 0) {
      printf("[t~%dms] availableToRead=%zu infoCalls=%d\n", i*100, mq.availableToRead(), g_infoCalls.load());
    }
  }
  printf("\n== got %d records, infoCalls=%d ==\n", got, g_infoCalls.load());
  fflush(stdout); _exit(0);
}

int main(int argc, char** argv) {
  setvbuf(stdout, nullptr, _IONBF, 0);
  g_cmd = argc > 1 ? (uint32_t)atoi(argv[1]) : 0;
  printf("== hal_stream5 (EventFlag readBlocking, cmd=%u) ==\n", g_cmd);
  android::hardware::configureRpcThreadpool(4, true);
  std::thread(protocol_thread).detach();
  android::hardware::joinRpcThreadpool();
  return 0;
}
