// hal_stream3.cpp — IMU FMQ streaming using fully hidl-gen-generated IImu + ISensorClient.
//
// No asm-binding: IImu::getService + imu->prepareStream/streamControl use generated marshalling,
// eliminating any hand-rolled ABI/layout risk in FmqConfig/MQDescriptor. getProperties() first
// as a sanity check that the typed proxy + connection fully work.

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
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
using android::hardware::hidl_handle;
using android::hardware::hidl_string;
using android::hardware::Return;
using android::hardware::Void;
namespace V1_0 = ::vendor::oculus::hardware::sensors::V1_0;

using ImuMQ = MessageQueue<V1_0::ImuData, kSynchronizedReadWrite>;

static volatile int g_infoCalls = 0;
struct MyClient : public V1_0::ISensorClient {
    Return<void> getSensorClientInfo(getSensorClientInfo_cb cb) override {
        g_infoCalls++;
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

int main(int argc, char** argv) {
  setvbuf(stdout, nullptr, _IONBF, 0);
  uint32_t cmd = argc > 1 ? (uint32_t)atoi(argv[1]) : 1;
  printf("== hal_stream3 (generated IImu, START cmd=%u) ==\n", cmd);

  android::hardware::configureRpcThreadpool(4, false);
  std::thread([]{ android::hardware::joinRpcThreadpool(); }).detach();

  sp<V1_0::IImu> imu = V1_0::IImu::getService("default", false);
  printf("IImu::getService -> %p\n", imu.get());
  if (!imu) return 1;

  // sanity: getProperties via the generated typed proxy
  imu->getProperties([](V1_0::Result r, const V1_0::MotionSensorProperties& p){
    printf("getProperties Result=%d sensor='%s' label='%s' rate=%g\n",
           (int)r, p.sensorType.c_str(), p.label.c_str(), p.rate);
  });

  ImuMQ mq(256, false);
  printf("mq.isValid=%d elem=%zu\n", mq.isValid(), sizeof(V1_0::ImuData));
  if (!mq.isValid()) return 2;

  sp<V1_0::ISensorClient> client = new MyClient();

  // eventflag handle — EXACT replica of OVR::OS::createEventFlagHandle: ashmem 4 bytes,
  // prot RW, native_handle(1 fd,0 ints), hidl_handle owns.
  int efd = ashmem_create_region("openvr-imu-evflag", 4);
  ashmem_set_prot_region(efd, PROT_READ | PROT_WRITE);
  native_handle_t* nh = native_handle_create(1, 0); nh->data[0] = efd;
  V1_0::FmqConfig cfg;
  cfg.eventFlag.setTo(nh, true /*own*/);
  cfg.bitA = 0x8000; cfg.bitB = 0x1;
  printf("eventflag fd=%d (4B, prot RW)\n", efd);

  auto r1 = imu->prepareStream(*mq.getDesc(), client, cfg);
  printf("prepareStream isOk=%d desc='%s'\n", r1.isOk(), r1.isOk() ? "" : r1.description().c_str());
  // real client uses prepareStream ALONE (no streamControl). Only call it if cmd != 99.
  if (cmd != 99) {
    auto r2 = imu->streamControl(client, (V1_0::StreamCommand)cmd);
    printf("streamControl(%u) isOk=%d\n", cmd, r2.isOk());
  }

  V1_0::ImuData rec; int got = 0;
  for (int i = 0; i < 5000 && got < 6; i++) {
    if (i % 500 == 0) printf("[t=%dms] availableToRead=%zu infoCalls=%d\n", i, mq.availableToRead(), g_infoCalls);
    while (mq.read(&rec, 1)) {
      printf("\n[rec %d]\n", got); hexdump(&rec, sizeof(rec));
      if (++got >= 6) break;
    }
    usleep(1000);
  }
  printf("\n== got %d records, infoCalls=%d ==\n", got, g_infoCalls);
  fflush(stdout); _exit(0);
}
