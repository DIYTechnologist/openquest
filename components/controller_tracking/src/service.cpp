// SPDX-License-Identifier: MIT
// A single authenticated camera/runtime producer owns each stream connection.
// This process never opens hardware, changes SELinux or injects Meta poses.
#include "protocol.hpp"
#include "tracker.hpp"
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <opencv2/calib3d.hpp>
#include <poll.h>
#include <set>
#include <signal.h>
#include <stdexcept>
#include <string>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#include <vector>

namespace {
volatile sig_atomic_t running = 1;
void stop(int) { running = 0; }
uint64_t now() {
  timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return uint64_t(t.tv_sec) * 1000000000 + t.tv_nsec;
}
bool read_exact(int fd, void *data, size_t size) {
  auto *p = static_cast<uint8_t *>(data);
  uint64_t deadline = now() + 1000000000;
  while (size && running) {
    pollfd poller{fd, POLLIN, 0};
    int ready = poll(&poller, 1, 100);
    if (ready < 0 && errno == EINTR)
      continue;
    if (ready < 0 || now() > deadline)
      return false;
    if (!ready)
      continue;
    ssize_t n = read(fd, p, size);
    if (n <= 0)
      return false;
    p += n;
    size -= n;
  }
  return size == 0;
}
bool write_exact(int fd, const void *data, size_t size) {
  auto *p = static_cast<const uint8_t *>(data);
  while (size && running) {
    ssize_t n = send(fd, p, size, MSG_NOSIGNAL);
    if (n < 0 && errno == EINTR)
      continue;
    if (n <= 0)
      return false;
    p += n;
    size -= n;
  }
  return size == 0;
}
struct Cursor {
  const std::vector<uint8_t> &data;
  size_t at = 0;
  template <class T> T get() {
    if (sizeof(T) > data.size() - at)
      throw std::runtime_error("Truncated payload");
    T value;
    std::memcpy(&value, data.data() + at, sizeof(T));
    at += sizeof(T);
    return value;
  }
};
bool near(uint64_t a, uint64_t b, uint64_t limit) {
  return a > b ? a - b <= limit : b - a <= limit;
}
std::vector<oq::View> parse_views(const oq::Config &config,
                                  const oq::wire::Header &header,
                                  const std::vector<uint8_t> &data) {
  Cursor p{data};
  uint32_t count = p.get<uint32_t>();
  if (count == 0 || count > 4)
    throw std::runtime_error("Invalid view count");
  std::set<uint32_t> seen;
  std::vector<oq::View> views;
  for (uint32_t i = 0; i < count; i++) {
    uint32_t camera = p.get<uint32_t>();
    if (camera > 3 || !seen.insert(camera).second)
      throw std::runtime_error("Invalid/duplicate camera");
    oq::View view{int(camera), {}};
    if (header.type == oq::wire::Bearings) {
      uint32_t rays = p.get<uint32_t>();
      uint64_t time = p.get<uint64_t>();
      if (rays > 256 || !near(time, header.timestamp_ns, 100000))
        throw std::runtime_error("Unsynchronized bearing view");
      for (uint32_t j = 0; j < rays; j++) {
        double x = p.get<double>(), y = p.get<double>(), z = p.get<double>();
        oq::V ray(x, y, z);
        double length = cv::norm(ray);
        if (!std::isfinite(length) || std::abs(length - 1) > .001)
          throw std::runtime_error("Invalid bearing");
        view.rays.push_back(ray);
      }
    } else {
      uint16_t width = p.get<uint16_t>(), height = p.get<uint16_t>();
      uint64_t time = p.get<uint64_t>();
      if (width != 640 || height != 480 ||
          !near(time, header.timestamp_ns, 100000) ||
          data.size() - p.at < 640 * 480)
        throw std::runtime_error("Invalid/unsynchronized image");
      cv::Mat image(480, 640, CV_8U, const_cast<uint8_t *>(data.data() + p.at));
      view.rays = oq::detect(image, config.cameras[camera]);
      p.at += 640 * 480;
    }
    views.push_back(std::move(view));
  }
  if (p.at != data.size())
    throw std::runtime_error("Trailing payload");
  return views;
}
void session(int fd, const oq::Config &config, bool fixed_head) {
  oq::Tracker tracker(config);
  oq::Gyro controller, head;
  std::vector<uint8_t> payload;
  uint64_t last_frame = 0;
  unsigned diagnostic_frames = 0;
  while (running) {
    oq::wire::Header header;
    if (!read_exact(fd, &header, sizeof(header)))
      return;
    if (header.magic != oq::wire::magic ||
        header.version != oq::wire::version || header.reserved ||
        header.bytes > oq::wire::maximum_payload || header.timestamp_ns == 0 ||
        !near(header.timestamp_ns, now(), 1000000000))
      throw std::runtime_error("Invalid/stale stream header");
    payload.resize(header.bytes);
    if (!read_exact(fd, payload.data(), payload.size()))
      return;
    if (header.type == oq::wire::ControllerGyro ||
        header.type == oq::wire::HeadGyro) {
      if (payload.size() != 3 * sizeof(double))
        throw std::runtime_error("Invalid gyro size");
      double raw[3];
      std::memcpy(raw, payload.data(), sizeof(raw));
      oq::V w(raw[0], raw[1], raw[2]);
      if (cv::norm(w) > 100)
        throw std::runtime_error("Implausible gyro");
      (header.type == oq::wire::ControllerGyro ? controller : head)
          .push(header.timestamp_ns * 1e-9, w);
    } else if (header.type == oq::wire::Bearings ||
               header.type == oq::wire::Images) {
      if (header.timestamp_ns <= last_frame)
        throw std::runtime_error("Nonmonotonic frame");
      last_frame = header.timestamp_ns;
      auto views = parse_views(config, header, payload);
      if (std::getenv("OQ_TRACKING_DIAGNOSTICS") && diagnostic_frames++ < 5) {
        std::cerr << "rays";
        for (auto &v : views)
          std::cerr << " cam" << v.camera << "=" << v.rays.size();
        std::cerr << "\n";
      }
      auto pose = tracker.update(header.timestamp_ns * 1e-9, views, controller,
                                 fixed_head ? nullptr : &head);
      oq::wire::Pose result{};
      result.timestamp_ns = header.timestamp_ns;
      result.flags = oq::wire::HeadRelative;
      if (pose.valid && near(header.timestamp_ns, now(), 100000000)) {
        result.flags |= oq::wire::PositionValid | oq::wire::OrientationValid;
        result.support = pose.support;
        result.error = pose.error;
        for (int i = 0; i < 3; i++)
          result.position[i] = pose.position[i];
        oq::V axis;
        cv::Rodrigues(pose.rotation, axis);
        double a = cv::norm(axis),
               scale = a < 1e-12 ? .5 : std::sin(a * .5) / a;
        for (int i = 0; i < 3; i++)
          result.quaternion_xyzw[i] = axis[i] * scale;
        result.quaternion_xyzw[3] = std::cos(a * .5);
      }
      oq::wire::Header response{oq::wire::magic,
                                oq::wire::version,
                                oq::wire::Result,
                                sizeof(result),
                                0,
                                header.timestamp_ns};
      if (!write_exact(fd, &response, sizeof(response)) ||
          !write_exact(fd, &result, sizeof(result)))
        return;
    } else
      throw std::runtime_error("Unknown packet type");
  }
}
} // namespace
int main(int argc, char **argv) {
  if (argc != 5 && argc != 6) {
    std::cerr << "usage: oq-controller-service CAMERAS MODEL GYRO_CALIBRATION "
                 "SOCKET [--fixed-head]\n";
    return 2;
  }
  try {
    bool fixed_head = argc == 6;
    if (fixed_head && std::string(argv[5]) != "--fixed-head")
      throw std::runtime_error("Unknown option");
    cv::setNumThreads(1);
    auto config = oq::load_config(argv[1], argv[2], argv[3]);
    signal(SIGTERM, stop);
    signal(SIGINT, stop);
    signal(SIGPIPE, SIG_IGN);
    int listener;
    bool inherited = false;
    const char *initfd = getenv("ANDROID_SOCKET_openquest_controller");
    if (initfd) {
      char *end;
      long n = strtol(initfd, &end, 10);
      if (*end || n < 0 || n > 100000)
        throw std::runtime_error("Invalid init socket");
      listener = n;
      inherited = true;
    } else {
      listener = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
      if (listener < 0)
        throw std::runtime_error("socket failed");
      sockaddr_un address{};
      address.sun_family = AF_UNIX;
      if (std::strlen(argv[4]) >= sizeof(address.sun_path))
        throw std::runtime_error("Socket path too long");
      std::strcpy(address.sun_path, argv[4]);
      // Do not unlink an unrelated pre-existing path. A standalone test must
      // select a fresh pathname; init owns the production socket lifecycle.
      mode_t previous = umask(0077);
      int bound = bind(listener, (sockaddr *)&address, sizeof(address));
      umask(previous);
      if (bound)
        throw std::runtime_error("bind failed");
    }
    if (listen(listener, 1))
      throw std::runtime_error("listen failed");
    while (running) {
      pollfd p{listener, POLLIN, 0};
      int ready = poll(&p, 1, 100);
      if (ready < 0 && errno == EINTR)
        continue;
      if (ready < 0)
        break;
      if (!ready)
        continue;
      int client = accept4(listener, nullptr, nullptr, SOCK_CLOEXEC);
      if (client < 0)
        continue;
      ucred credentials{};
      socklen_t length = sizeof(credentials);
      if (getsockopt(client, SOL_SOCKET, SO_PEERCRED, &credentials, &length) ||
          !(credentials.uid == 0 || credentials.uid == 1000)) {
        close(client);
        continue;
      }
      timeval timeout{1, 0};
      setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
      try {
        session(client, config, fixed_head);
      } catch (const std::exception &e) {
        std::cerr << "producer disconnected: " << e.what() << '\n';
      }
      close(client);
    }
    close(listener);
    if (!inherited)
      unlink(argv[4]);
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
  return 0;
}
