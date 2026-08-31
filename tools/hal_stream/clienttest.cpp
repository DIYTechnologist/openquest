// clienttest.cpp — prove our generated ISensorClient Bn/Bp round-trips over REAL binder IPC.
//   server: register MyClient under ISensorClient/"testclient", serve forever.
//   client: getService("testclient"), call getSensorClientInfo, print what comes back.
// If the client sees name="openvr", our ISensorClient server object is fully callable across
// processes — meaning the HAL's inability to call getSensorClientInfo on us is NOT a broken-Bn
// problem but a registration/routing decision inside the HAL.

#include <cstdio>
#include <cstring>
#include <string>
#include <unistd.h>

#include <hidl/HidlTransportSupport.h>
#include <android/hidl/manager/1.0/IServiceManager.h>
#include <vendor/oculus/hardware/sensors/1.0/ISensorClient.h>

using android::sp;
using android::hardware::Return;
using android::hardware::Void;
namespace V1_0 = ::vendor::oculus::hardware::sensors::V1_0;

struct MyClient : public V1_0::ISensorClient {
    Return<void> getSensorClientInfo(getSensorClientInfo_cb cb) override {
        fprintf(stderr, "[server: getSensorClientInfo invoked]\n");
        V1_0::SensorClientInfo info; info.name = "openvr";
        info.field0 = 11; info.field1 = 22; info.field2 = 33;
        cb(V1_0::Result::OK, info);
        return Void();
    }
};

int main(int argc, char** argv) {
  setvbuf(stdout, nullptr, _IONBF, 0);
  bool server = argc > 1 && !strcmp(argv[1], "server");

  if (server) {
    android::hardware::configureRpcThreadpool(4, true);
    sp<MyClient> c = new MyClient();
    auto st = c->registerAsService("testclient");
    printf("registerAsService -> %d (0=OK)\n", (int)st);
    android::hardware::joinRpcThreadpool();
    return 0;
  }

  // client: first grab the RAW binder from hwservicemanager (no cast verification), probe its
  // advertised interfaceChain/descriptor, THEN try the typed getService.
  {
    auto sm = ::android::hardware::defaultServiceManager();
    if (sm != nullptr) {
      using ::android::hidl::base::V1_0::IBase;
      sp<IBase> base = sm->get("vendor.oculus.hardware.sensors@1.0::ISensorClient", "testclient");
      printf("raw sm->get -> %p\n", base.get());
      if (base != nullptr) {
        base->interfaceDescriptor([](const android::hardware::hidl_string& d){
          printf("interfaceDescriptor='%s'\n", d.c_str());
        });
        base->interfaceChain([](const auto& chain){
          printf("interfaceChain:\n");
          for (const auto& s : chain) printf("  '%s'\n", s.c_str());
        });
        // THE EXACT OP THE HAL DOES on the client it receives in prepareStream:
        sp<V1_0::ISensorClient> cast = V1_0::ISensorClient::castFrom(base, /*emitError*/true);
        printf("ISensorClient::castFrom(base) -> %p\n", cast.get());
        if (cast != nullptr) {
          cast->getSensorClientInfo([](V1_0::Result r, const V1_0::SensorClientInfo& info){
            printf("CAST CALLBACK OK: name='%s' f0=%llu\n", info.name.c_str(),
                   (unsigned long long)info.field0);
          });
        }
      }
    }
  }
  sp<V1_0::ISensorClient> c;
  for (int i = 0; i < 5 && c == nullptr; i++) {
    c = V1_0::ISensorClient::getService("testclient", false);
    if (!c) { printf("typed getService try %d -> null\n", i); usleep(200000); }
  }
  printf("getService(testclient) -> %p\n", c.get());
  if (!c) return 1;
  bool got = false;
  c->getSensorClientInfo([&](V1_0::Result r, const V1_0::SensorClientInfo& info){
    got = true;
    printf("CALLBACK OK: Result=%d name='%s' f0=%llu f1=%llu f2=%llu\n",
           (int)r, info.name.c_str(),
           (unsigned long long)info.field0, (unsigned long long)info.field1,
           (unsigned long long)info.field2);
  });
  printf("round-trip %s\n", got ? "SUCCEEDED" : "FAILED");
  return got ? 0 : 2;
}
