// SPDX-License-Identifier: MIT
#include "../../camera/src/syncboss_framer.h"
#include "live_input.hpp"
#include <iostream>
#include <stdexcept>
#include <vector>
using namespace oq;
static void require(bool b, const char *m) {
  if (!b)
    throw std::runtime_error(m);
}
struct Packets {
  std::vector<std::vector<uint8_t>> received;
};
static void packet(void *context, uint8_t kind, const uint8_t *p, uint8_t n) {
  auto &out = *(Packets *)context;
  std::vector<uint8_t> bytes{kind};
  bytes.insert(bytes.end(), p, p + n);
  out.received.push_back(bytes);
}
int main() {
  try {
    std::vector<uint8_t> stream{9, 8, 1, 3, 0, 0x8f, 0, 255};
    for (int i = 0; i < 255; i++)
      stream.push_back(i);
    stream.insert(stream.end(), {1, 3, 0, 0x50, 0, 3, 5, 6, 7});
    for (size_t split = 0; split <= stream.size(); split++) {
      oq_sb_framer state{};
      Packets out;
      oq_sb_feed(&state, stream.data(), split, packet, &out);
      oq_sb_feed(&state, stream.data() + split, stream.size() - split, packet,
                 &out);
      require(out.received.size() == 2 && out.received[0].size() == 256 &&
                  out.received[1] == std::vector<uint8_t>({0x50, 5, 6, 7}),
              "Packet split lost/corrupted samples");
    }
    Counter32 counter;
    require(counter.observe(0xfffffff0).value() == 0xfffffff0ULL,
            "Counter first value incorrect");
    require(counter.observe(32).value() == 0x100000020ULL, "Counter wrap lost");
    require(!counter.observe(31), "Reordered sample accepted");
    require(counter.near(0xfffffff8).value() == 0xfffffff8ULL,
            "Old exposure around wrap mapped incorrectly");
    ReceiptClock clock;
    for (uint64_t i = 0; i <= 600; i++) {
      uint64_t us = 1000000 + i * 1000,
               receipt = us * 1000 + 1000000000000ULL + (i % 2 ? 2000000 : 0);
      clock.observe(us, receipt);
      if (i < 400)
        require(!clock.valid(), "Clock used future prefix");
    }
    require(clock.valid() && clock.map(1600000) == 1001600000000ULL,
            "Clock biased by delayed reads");
    auto before = clock.map(1600000);
    clock.observe(2000000, 1009000000000ULL);
    require(clock.map(1600000) == before,
            "Frozen prefix changed after future input");
    ExposureMatcher matcher;
    std::optional<uint64_t> result;
    for (uint32_t i = 0; i < 3; i++) {
      uint64_t us = 1700000 + i * 40000;
      matcher.push({us, 100 + i});
      result = matcher.match(uint8_t(7 + 2 * i), clock.map(us) + 8000000, clock,
                             true);
      if (i < 2)
        require(!result, "Unconfirmed exposure correspondence published");
    }
    require(result && *result == clock.map(1780000),
            "Exposure counter alignment failed");
    require(!matcher.match(12, clock.map(1780000) + 8000000, clock, false),
            "Wrong embedded counter paired");
    require(!matcher.match(11, clock.map(1780000) + 30000000, clock, false),
            "Wrong strobe cycle paired");
    uint32_t next = 225;
    uint64_t us = 1900000;
    matcher.push({us, next});
    require(matcher.match(uint8_t(2 * next + 63), clock.map(us) + 8000000,
                          clock, false)
                    .value() == clock.map(us),
            "Embedded 8-bit counter wrap failed");
    std::cout
        << "PASS: every packet split, uint32 wrap/reorder, causal clock "
           "prefix, exposure-counter confirmation and wrong-cycle rejection\n";
  } catch (const std::exception &e) {
    std::cerr << "FAIL: " << e.what() << '\n';
    return 1;
  }
  return 0;
}
