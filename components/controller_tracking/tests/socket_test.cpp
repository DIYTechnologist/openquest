// SPDX-License-Identifier: MIT
#include "protocol.hpp"
#include <cmath>
#include <cstring>
#include <iostream>
#include <poll.h>
#include <stdexcept>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <vector>
using namespace oq;
static uint64_t now() {
  timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return uint64_t(t.tv_sec) * 1000000000 + t.tv_nsec;
}
static void require(bool b, const char *s) {
  if (!b)
    throw std::runtime_error(s);
}
static void send_all(int fd, const void *p, size_t n) {
  auto *b = (const char *)p;
  while (n) {
    ssize_t k = send(fd, b, n, MSG_NOSIGNAL);
    require(k > 0, "send failed");
    b += k;
    n -= k;
  }
}
static void receive(int fd, void *p, size_t n) {
  auto *b = (char *)p;
  while (n) {
    pollfd event{fd, POLLIN, 0};
    require(poll(&event, 1, 2000) > 0, "receive timeout");
    ssize_t k = read(fd, b, n);
    require(k > 0, "unexpected disconnect");
    b += k;
    n -= k;
  }
}
static int connect_to(const char *path) {
  int fd = socket(AF_UNIX, SOCK_STREAM, 0);
  sockaddr_un address{};
  address.sun_family = AF_UNIX;
  require(strlen(path) < sizeof(address.sun_path), "path too long");
  strcpy(address.sun_path, path);
  require(connect(fd, (sockaddr *)&address, sizeof(address)) == 0,
          "connect failed");
  return fd;
}
static void packet(int fd, uint16_t type, uint64_t time, const void *data,
                   uint32_t bytes) {
  wire::Header h{wire::magic, wire::version, type, bytes, 0, time};
  send_all(fd, &h, sizeof(h));
  if (bytes)
    send_all(fd, data, bytes);
}
template <class T> static void append(std::vector<uint8_t> &out, T v) {
  auto *p = (uint8_t *)&v;
  out.insert(out.end(), p, p + sizeof(v));
}
static wire::Pose frame(int fd, uint64_t timestamp) {
  const double model[][3] = {
      {-.04, -.03, 0}, {.04, -.03, .005}, {.04, .03, 0},    {-.04, .03, .005},
      {0, .05, -.01},  {.05, 0, .01},     {-.02, .01, .012}};
  double center[3] = {};
  for (auto &p : model)
    for (int k = 0; k < 3; k++)
      center[k] += p[k] / 7;
  std::vector<uint8_t> body;
  append(body, uint32_t(4));
  for (uint32_t camera = 0; camera < 4; camera++) {
    append(body, camera);
    append(body, uint32_t(7));
    append(body, timestamp);
    for (auto &p : model) {
      double v[] = {p[0] - center[0] + .02 - (camera - 1.5) * .05,
                    p[1] - center[1] - .01, p[2] - center[2] + .7};
      double length = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
      for (double x : v)
        append(body, x / length);
    }
  }
  packet(fd, wire::Bearings, timestamp, body.data(), body.size());
  wire::Header h;
  wire::Pose p;
  receive(fd, &h, sizeof(h));
  require(h.magic == wire::magic && h.version == wire::version &&
              h.type == wire::Result && h.bytes == sizeof(p),
          "bad response header");
  receive(fd, &p, sizeof(p));
  require(p.timestamp_ns == timestamp, "wrong result time");
  return p;
}
int main(int argc, char **argv) {
  if (argc != 2)
    return 2;
  try {
    int fd = connect_to(argv[1]);
    uint64_t sensor = now() - 4000000;
    int valid = 0;
    for (int i = 0; i < 32; i++) {
      uint64_t time = now();
      double omega[3] = {0, 0, 0};
      while (sensor <= time + 2000000) {
        packet(fd, wire::ControllerGyro, sensor, omega, sizeof(omega));
        packet(fd, wire::HeadGyro, sensor, omega, sizeof(omega));
        sensor += 2000000;
      }
      auto p = frame(fd, time);
      if (p.flags & wire::PositionValid) {
        valid++;
        require((p.flags & (wire::OrientationValid | wire::HeadRelative)) ==
                    (wire::OrientationValid | wire::HeadRelative),
                "pose flags wrong");
        require(std::abs(p.position[0] - .02) < 1e-4 &&
                    std::abs(p.position[1] + .01) < 1e-4 &&
                    std::abs(p.position[2] - .7) < 1e-4,
                "socket pose inaccurate");
      }
      usleep(40000);
    }
    require(valid >= 15, "no usable socket tracking");
    usleep(30000);
    auto missing = frame(fd, now());
    require(!(missing.flags & (wire::PositionValid | wire::OrientationValid)),
            "stale gyro reused as valid tracking");
    close(fd);
    fd = connect_to(argv[1]);
    auto reset = frame(fd, now());
    require(!(reset.flags & wire::PositionValid),
            "reconnect retained old tracking");
    close(fd);
    for (bool oversized : {false, true}) {
      fd = connect_to(argv[1]);
      wire::Header bad{wire::magic,
                       wire::version,
                       wire::Bearings,
                       oversized ? wire::maximum_payload + 1 : 0,
                       0,
                       now() - (oversized ? 0 : 2000000000ULL)};
      send_all(fd, &bad, sizeof(bad));
      pollfd event{fd, POLLIN, 0};
      require(poll(&event, 1, 1000) > 0, "invalid header not rejected");
      char byte;
      require(read(fd, &byte, 1) <= 0, "invalid header returned data");
      close(fd);
    }
    std::cout << "PASS: live socket geometry (" << valid
              << " poses), gyro loss invalidation, reconnect reset, "
                 "stale/oversized message rejection\n";
  } catch (const std::exception &e) {
    std::cerr << "FAIL: " << e.what() << '\n';
    return 1;
  }
  return 0;
}
