// SPDX-License-Identifier: MIT
// Open cam_kernel controller stream -> calibrated clocks / grouped exposures ->
// the versioned tracking service. No Meta userspace library or pose input.
#include "live_input.hpp"
#include "protocol.hpp"
#include <atomic>
#include <cerrno>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <mutex>
#include <opencv2/core.hpp>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <thread>
#include <unistd.h>

namespace {
using namespace oq;
#pragma pack(push, 1)
struct Record {
  uint8_t type, camera;
  uint16_t width, height;
  uint64_t device_ns, host_ns;
  uint32_t bytes;
};
#pragma pack(pop)
static_assert(sizeof(Record) == 26);
uint64_t now() {
  timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return uint64_t(t.tv_sec) * 1000000000 + t.tv_nsec;
}
template <class T> T value(const uint8_t *p) {
  T v;
  std::memcpy(&v, p, sizeof(v));
  return v;
}
struct RawSample {
  uint64_t us;
  V omega;
};
struct Batch {
  uint64_t time = 0;
  std::array<cv::Mat, 4> images;
  std::array<bool, 4> present{};
};
struct Shared {
  std::mutex mutex;
  std::condition_variable changed;
  bool done = false;
  std::string error;
  ReceiptClock controller_clock, head_clock;
  Counter32 head_counter;
  ExposureMatcher exposures;
  std::deque<RawSample> controller, head;
  std::optional<Batch> pending;
  uint64_t controller_last = 0, last_batch = 0, frames = 0,
           controller_packets = 0, controller_left_packets = 0,
           controller_unknown_packets = 0, head_packets = 0, overwritten = 0,
           unmatched = 0, warmup = 0, stale = 0, matched = 0;
};
void sample(std::deque<RawSample> &out, RawSample s) {
  if (!out.empty() && s.us <= out.back().us)
    return;
  out.push_back(s);
  while (out.size() > 6000)
    out.pop_front();
}
R matrix(cv::FileNode n) {
  if (n.size() != 9)
    throw std::runtime_error("Invalid head gyro matrix");
  R out;
  for (int i = 0; i < 9; i++)
    out.val[i] = (double)n[i];
  return out;
}
V vector(cv::FileNode n) {
  if (n.size() != 3)
    throw std::runtime_error("Invalid head gyro bias");
  return V((double)n[0], (double)n[1], (double)n[2]);
}
void ingest_sensor(Shared &shared, const Record &r,
                   const std::vector<uint8_t> &p, const Config &c,
                   R head_matrix, V head_bias) {
  if (r.camera == 0x50 && p.size() == 36) {
    uint32_t raw = value<uint32_t>(p.data());
    auto time = shared.head_counter.observe(raw);
    if (!time)
      return;
    V w(value<float>(p.data() + 20), value<float>(p.data() + 24),
        value<float>(p.data() + 28));
    w = head_matrix * (w * (CV_PI / 180) - head_bias);
    if (!cv::checkRange(cv::Mat(w)))
      throw std::runtime_error("Nonfinite head gyro");
    shared.head_clock.observe(*time, r.host_ns);
    sample(shared.head, {*time, w});
    shared.head_packets++;
  } else if (r.camera == 0xe0 && p.size() == 14) {
    auto time = shared.head_counter.near(value<uint32_t>(p.data() + 1));
    if (time)
      shared.exposures.push({*time, value<uint32_t>(p.data() + 9)});
  } else if (r.camera == 0x8f) {
    if (p.size() < 23 || p[10] > 1) {
      shared.controller_unknown_packets++;
      return;
    }
    if (p[10] == 0) {
      shared.controller_left_packets++;
      return;
    }
    shared.controller_packets++;
    for (size_t i = 23; i + 2 <= p.size();) {
      uint8_t tag = p[i], marker = p[i + 1];
      if (tag == 0x41 && marker == 0x82) {
        if (p.size() - i < 20)
          return;
        uint64_t us = 0;
        for (int j = 0; j < 6; j++)
          us |= uint64_t(p[i + 2 + j]) << (j * 8);
        if (us > shared.controller_last) {
          shared.controller_last = us;
          V raw(value<int16_t>(p.data() + i + 14),
                value<int16_t>(p.data() + i + 16),
                value<int16_t>(p.data() + i + 18));
          V w = c.gyro_matrix * (raw * c.gyro_scale - c.gyro_bias);
          shared.controller_clock.observe(us, r.host_ns);
          sample(shared.controller, {us, w});
        }
        i += 20;
      } else if (marker == 0x80) {
        size_t n = 0;
        switch (tag) {
        case 0x24:
        case 0x25:
        case 0x26:
        case 0x23:
        case 0x22:
        case 0x21:
          n = 1;
          break;
        case 0x87:
        case 0x82:
          n = 4;
          break;
        case 0x63:
          n = 3;
          break;
        default:
          return;
        }
        if (p.size() - i < 2 + n)
          return;
        i += 2 + n;
      } else
        return;
    }
  }
}
bool read_exact(void *output, size_t bytes, bool allow_eof = false) {
  auto *p = static_cast<uint8_t *>(output);
  size_t done = 0;
  while (done < bytes) {
    ssize_t n = ::read(STDIN_FILENO, p + done, bytes - done);
    if (n < 0 && errno == EINTR)
      continue;
    if (n == 0 && done == 0 && allow_eof)
      return false;
    if (n <= 0)
      throw std::runtime_error("Truncated/unreadable camera stream");
    done += size_t(n);
  }
  return true;
}
bool read_record(Record &r, std::vector<uint8_t> &p) {
  if (!read_exact(&r, sizeof(r), true))
    return false;
  if (r.bytes > 640 * 481)
    throw std::runtime_error("Oversized camera record");
  p.resize(r.bytes);
  read_exact(p.data(), p.size());
  return true;
}
void reader(Shared &shared, const Config &c, R hm, V hb) {
  try {
    std::map<uint64_t, Batch> assembling;
    Record r;
    std::vector<uint8_t> p;
    while (read_record(r, p)) {
      std::lock_guard<std::mutex> lock(shared.mutex);
      if (r.type == 'S') {
        ingest_sensor(shared, r, p, c, hm, hb);
        continue;
      }
      if (r.type != 'R')
        throw std::runtime_error("Expected controller-mode R/S stream");
      if (r.camera > 3 || r.width != 640 || r.height != 481 ||
          p.size() != 640 * 481)
        throw std::runtime_error("Invalid camera view");
      shared.frames++;
      if (!shared.head_clock.valid() || !shared.controller_clock.valid()) {
        shared.warmup++;
        continue;
      }
      if (p[3] != p[4] || p[4] != p[5])
        throw std::runtime_error("Invalid exposure metadata");
      unsigned lines = (unsigned(p[6]) << 8) | p[7];
      if (lines == 0 || lines > 4)
        throw std::runtime_error("Not a short controller exposure");
      auto exposure = shared.exposures.match(p[89], r.device_ns,
                                             shared.head_clock, r.camera == 0);
      if (!exposure) {
        shared.unmatched++;
        continue;
      }
      shared.matched++;
      if (*exposure <= shared.last_batch || now() > *exposure + 80000000) {
        shared.stale++;
        continue;
      }
      auto &batch = assembling[*exposure];
      batch.time = *exposure;
      batch.images[r.camera] = cv::Mat(480, 640, CV_8U, p.data() + 640).clone();
      batch.present[r.camera] = true;
      if (std::all_of(batch.present.begin(), batch.present.end(),
                      [](bool v) { return v; })) {
        if (shared.pending)
          shared.overwritten++;
        shared.pending = std::move(batch);
        shared.last_batch = *exposure;
        assembling.erase(*exposure);
        shared.changed.notify_one();
      }
      while (assembling.size() > 4)
        assembling.erase(assembling.begin());
    }
  } catch (const std::exception &e) {
    std::lock_guard<std::mutex> lock(shared.mutex);
    shared.error = e.what();
  }
  {
    std::lock_guard<std::mutex> lock(shared.mutex);
    shared.done = true;
  }
  shared.changed.notify_all();
}
void send_all(int fd, const void *bytes, size_t n) {
  auto *p = (const char *)bytes;
  while (n) {
    ssize_t k = send(fd, p, n, MSG_NOSIGNAL);
    if (k < 0 && errno == EINTR)
      continue;
    if (k <= 0)
      throw std::runtime_error("Tracking service disconnected");
    p += k;
    n -= k;
  }
}
void receive(int fd, void *bytes, size_t n) {
  auto *p = (char *)bytes;
  while (n) {
    pollfd state{fd, POLLIN, 0};
    if (poll(&state, 1, 1000) <= 0)
      throw std::runtime_error("Tracking service response timed out");
    ssize_t k = read(fd, p, n);
    if (k <= 0)
      throw std::runtime_error("Tracking service disconnected");
    p += k;
    n -= k;
  }
}
void packet(int fd, uint16_t type, uint64_t time, const void *p, uint32_t n) {
  wire::Header h{wire::magic, wire::version, type, n, 0, time};
  send_all(fd, &h, sizeof(h));
  send_all(fd, p, n);
}
template <class T> void append(std::vector<uint8_t> &out, T v) {
  auto *p = (const uint8_t *)&v;
  out.insert(out.end(), p, p + sizeof(v));
}
std::vector<Sample> convert(const std::deque<RawSample> &raw,
                            const ReceiptClock &clock, double correction,
                            uint64_t after) {
  std::vector<Sample> out;
  for (auto s : raw) {
    uint64_t ns = clock.map(s.us, correction);
    if (ns > after)
      out.push_back({ns * 1e-9, s.omega});
  }
  return out;
}
} // namespace
int main(int argc, char **argv) {
  std::ios::sync_with_stdio(false);
  std::cin.tie(nullptr);
  if (argc != 7) {
    std::cerr << "usage: oq-controller-live CAMERAS MODEL GYRO_CALIBRATION "
                 "HEAD_FACTORY_CALIBRATION SOCKET OUTPUT_CSV\n";
    return 2;
  }
  try {
    auto config = oq::load_config(argv[1], argv[2], argv[3]);
    cv::FileStorage h(argv[4], cv::FileStorage::READ);
    if (!h.isOpened())
      throw std::runtime_error("Missing private head calibration");
    auto node = h["ImuCalibration"]["Gyroscope"];
    R hm = matrix(node["RectificationMatrix"]);
    V hb = vector(node["Offset"]["ConstantOffset"]);
    int fd = -1;
    std::ofstream output(argv[6]);
    if (!output)
      throw std::runtime_error("Cannot open output");
    output << "#pose_reference=centroid; live direct camera input; no Meta "
              "userspace\n"
           << std::setprecision(17);
    Shared shared;
    std::thread input(reader, std::ref(shared), std::cref(config), hm, hb);
    uint64_t sent_c = 0, sent_h = 0, solved = 0, total = 0;
    // Closing the process on transport failure closes the pipe too, allowing
    // the bounded capture supervisor to stop/recover its hardware owner.
    try {
      while (true) {
        Batch batch;
        std::vector<Sample> controller, head;
        {
          std::unique_lock<std::mutex> lock(shared.mutex);
          shared.changed.wait_for(lock, std::chrono::milliseconds(100), [&] {
            return shared.done || shared.pending.has_value();
          });
          if (!shared.pending) {
            if (shared.done)
              break;
            continue;
          }
          batch = std::move(*shared.pending);
          shared.pending.reset();
          controller = convert(shared.controller, shared.controller_clock,
                               config.gyro_time_correction,
                               std::max(sent_c, batch.time - 500000000));
          head = convert(shared.head, shared.head_clock, 0,
                         std::max(sent_h, batch.time - 500000000));
        }
        if (now() > batch.time + 80000000)
          continue;
        if (fd < 0) {
          fd = socket(AF_UNIX, SOCK_STREAM, 0);
          sockaddr_un address{};
          address.sun_family = AF_UNIX;
          if (std::strlen(argv[5]) >= sizeof(address.sun_path))
            throw std::runtime_error("Socket path too long");
          std::strcpy(address.sun_path, argv[5]);
          if (connect(fd, (sockaddr *)&address, sizeof(address)))
            throw std::runtime_error("Cannot connect tracking service");
        }
        for (auto s : controller) {
          uint64_t time = uint64_t(std::llround(s.time * 1e9));
          packet(fd, wire::ControllerGyro, time, s.omega.val,
                 sizeof(s.omega.val));
          sent_c = time;
        }
        for (auto s : head) {
          uint64_t time = uint64_t(std::llround(s.time * 1e9));
          packet(fd, wire::HeadGyro, time, s.omega.val, sizeof(s.omega.val));
          sent_h = time;
        }
        std::vector<uint8_t> body;
        body.reserve(wire::maximum_payload);
        append(body, uint32_t(4));
        for (uint32_t cam = 0; cam < 4; cam++) {
          append(body, cam);
          append(body, uint16_t(640));
          append(body, uint16_t(480));
          append(body, batch.time);
          body.insert(body.end(), batch.images[cam].data,
                      batch.images[cam].data + 640 * 480);
        }
        packet(fd, wire::Images, batch.time, body.data(), body.size());
        wire::Header response;
        wire::Pose pose;
        receive(fd, &response, sizeof(response));
        if (response.magic != wire::magic ||
            response.version != wire::version ||
            response.type != wire::Result || response.bytes != sizeof(pose))
          throw std::runtime_error("Invalid pose response");
        receive(fd, &pose, sizeof(pose));
        total++;
        if ((pose.flags & (wire::PositionValid | wire::OrientationValid)) ==
            (wire::PositionValid | wire::OrientationValid)) {
          solved++;
          output << pose.timestamp_ns << ",0," << pose.support << ','
                 << pose.error << ",live," << pose.position[0] << ','
                 << pose.position[1] << ',' << pose.position[2];
          for (double q : pose.quaternion_xyzw)
            output << ',' << q;
          output << '\n';
        } else
          output << pose.timestamp_ns << ",0,0,,,,,,,,,\n";
        output.flush();
      }
    } catch (const std::exception &e) {
      std::cerr << e.what() << '\n';
      std::_Exit(1);
    }
    input.join();
    close(fd);
    std::cerr << "frames=" << shared.frames
              << " head_samples=" << shared.head.size()
              << " controller_samples=" << shared.controller.size()
              << " head_clock=" << shared.head_clock.valid()
              << " controller_clock=" << shared.controller_clock.valid()
              << " head_packets=" << shared.head_packets
              << " controller_packets=" << shared.controller_packets
              << " left_packets=" << shared.controller_left_packets
              << " unknown_controller_packets="
              << shared.controller_unknown_packets << " sent=" << total
              << " solved=" << solved << " overwritten=" << shared.overwritten
              << " warmup=" << shared.warmup << " matched=" << shared.matched
              << " stale=" << shared.stale << " unmatched=" << shared.unmatched
              << '\n';
    if (!shared.error.empty())
      throw std::runtime_error(shared.error);
    if (total == 0)
      throw std::runtime_error("No synchronized frame sets with both IMUs");
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
  return 0;
}
