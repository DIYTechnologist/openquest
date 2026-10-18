// SPDX-License-Identifier: MIT
#pragma once
#include "tracker.hpp"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <vector>
namespace oq {
// Causal initial clock alignment. Receipt lag is positive, so use the lower
// envelope of a half-second prefix, not its latency-biased average. The offset
// is frozen for this bounded bring-up session; long-run drift remains
// unvalidated.
class ReceiptClock {
public:
  bool observe(uint64_t us, uint64_t receipt) {
    if (first == 0)
      first = receipt;
    if (!ready) {
      offsets.push_back(double(receipt) - double(us) * 1000);
      if (receipt >= first + 500000000 && offsets.size() >= 20) {
        std::sort(offsets.begin(), offsets.end());
        double x = .01 * (offsets.size() - 1);
        size_t i = x;
        offset = offsets[i] +
                 (offsets[std::min(i + 1, offsets.size() - 1)] - offsets[i]) *
                     (x - i);
        ready = true;
        offsets.clear();
      }
    }
    return ready;
  }
  bool valid() const { return ready; }
  uint64_t map(uint64_t us, double correction_s = 0) const {
    double ns = double(us) * 1000 + offset + correction_s * 1e9;
    if (!ready || !std::isfinite(ns) || ns < 1 || ns > double(UINT64_MAX))
      throw std::runtime_error("Uninitialized/invalid sensor clock");
    return uint64_t(std::llround(ns));
  }

private:
  uint64_t first = 0;
  double offset = 0;
  bool ready = false;
  std::vector<double> offsets;
};
class Counter32 {
public:
  std::optional<uint64_t> observe(uint32_t raw) {
    if (have) {
      int32_t change = int32_t(raw - last_raw);
      if (change <= 0)
        return std::nullopt;
      unwrapped += uint32_t(change);
    } else {
      unwrapped = raw;
      have = true;
    }
    last_raw = raw;
    return unwrapped;
  }
  std::optional<uint64_t> near(uint32_t raw) const {
    if (!have)
      return std::nullopt;
    int64_t value = int64_t(unwrapped) + int32_t(raw - last_raw);
    if (value < 0)
      return std::nullopt;
    return uint64_t(value);
  }

private:
  bool have = false;
  uint32_t last_raw = 0;
  uint64_t unwrapped = 0;
};
struct ExposureEvent {
  uint64_t sensor_us;
  uint32_t counter;
};
class ExposureMatcher {
public:
  void push(ExposureEvent e) {
    events.push_back(e);
    if (events.size() > 128)
      events.erase(events.begin());
  }
  std::optional<uint64_t> match(uint8_t frame, uint64_t completion,
                                const ReceiptClock &clock, bool learn) {
    if (!clock.valid())
      return std::nullopt;
    const ExposureEvent *found = nullptr;
    for (auto &e : events) {
      uint64_t time = clock.map(e.sensor_us);
      // Camera V4L2 stamps are buffer-completion times, not exposure instants.
      // Require a unique recent hardware strobe, then confirm the independently
      // observed relationship between MCU strobe and embedded sensor counters.
      if (completion < time || completion - time > 15000000)
        continue;
      uint8_t relation = uint8_t(frame - 2 * e.counter);
      if (locked && relation != offset)
        continue;
      if (found)
        return std::nullopt;
      found = &e;
    }
    if (!found)
      return std::nullopt;
    uint8_t relation = uint8_t(frame - 2 * found->counter);
    if (!locked && learn) {
      if (votes && relation == offset)
        ++votes;
      else {
        offset = relation;
        votes = 1;
      }
      if (votes >= 3)
        locked = true;
    }
    return locked ? std::optional<uint64_t>(clock.map(found->sensor_us))
                  : std::nullopt;
  }
  bool valid() const { return locked; }

private:
  std::vector<ExposureEvent> events;
  bool locked = false;
  uint8_t offset = 0;
  unsigned votes = 0;
};
} // namespace oq
