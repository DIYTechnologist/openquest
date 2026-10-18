// SPDX-License-Identifier: MIT
// Native Android replay harness. Does not control cameras or inject poses.
#include "json_integer.hpp"
#include "tracker.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <opencv2/calib3d.hpp>
#include <sstream>
#include <stdexcept>
#include <string>

using namespace oq;
struct Index {
  int camera;
  uint64_t time, offset;
  uint32_t size;
  double exposure;
};
struct Timed {
  double receipt, sensor;
  V value;
};
static double percentile(std::vector<double> v, double q) {
  if (v.empty())
    throw std::runtime_error("Empty clock prefix");
  std::sort(v.begin(), v.end());
  double x = q * (v.size() - 1);
  size_t i = x;
  return v[i] + (v[std::min(i + 1, v.size() - 1)] - v[i]) * (x - i);
}
static std::vector<Sample> read_imu(const std::string &path,
                                    const Config &config, bool head,
                                    double &warmup) {
  std::ifstream in(path);
  if (!in)
    throw std::runtime_error("Cannot read IMU");
  std::string line;
  std::vector<Timed> raw;
  while (std::getline(in, line)) {
    cv::FileStorage f(line, cv::FileStorage::READ | cv::FileStorage::MEMORY |
                                cv::FileStorage::FORMAT_JSON);
    if (!head && (std::string)f["hand"] != "right")
      continue;
    double receipt = oq_json_timestamp(line, "receipt_mono_ns") * 1e-9,
           sensor = oq_json_timestamp(line, "sensor_us") * 1e-6;
    auto node = f[head ? "omega_rad_s" : "gyro_raw"];
    if (node.size() != 3)
      throw std::runtime_error("Invalid IMU sample");
    V w((double)node[0], (double)node[1], (double)node[2]);
    if (!head)
      w = config.gyro_matrix * (w * config.gyro_scale - config.gyro_bias);
    raw.push_back({receipt, sensor, w});
  }
  if (raw.empty())
    throw std::runtime_error("No requested IMU samples");
  warmup = raw.front().receipt + .5;
  std::vector<double> offsets;
  for (auto r : raw)
    if (r.receipt < warmup)
      offsets.push_back(r.receipt - r.sensor);
  double offset =
      percentile(offsets, .01) + (head ? 0 : config.gyro_time_correction);
  std::vector<Sample> out;
  double previous = -1;
  for (auto r : raw) {
    if (r.sensor <= previous)
      continue;
    previous = r.sensor;
    out.push_back({r.sensor + offset, r.value});
  }
  if (out.size() < 2)
    throw std::runtime_error("Insufficient monotonic IMU samples");
  return out;
}
static cv::Vec4d quaternion(R r) {
  V v;
  cv::Rodrigues(r, v);
  double a = cv::norm(v);
  if (a < 1e-12)
    return {0, 0, 0, 1};
  double s = std::sin(a * .5) / a;
  return {v[0] * s, v[1] * s, v[2] * s, std::cos(a * .5)};
}
int main(int argc, char **argv) {
  if (argc != 9) {
    std::cerr
        << "usage: oq-controller-replay CAPTURE CAMERAS MODEL GYRO_CALIBRATION "
           "CONTROLLER_IMU HEAD_IMU_OR_DASH OUTPUT --recorded\n";
    return 2;
  }
  try {
    cv::setNumThreads(1);
    auto c = load_config(argv[2], argv[3], argv[4]);
    double warmup;
    auto controller = read_imu(argv[5], c, false, warmup);
    std::vector<Sample> head;
    bool compensate = std::string(argv[6]) != "-";
    if (compensate) {
      double hw;
      head = read_imu(argv[6], c, true, hw);
      warmup = std::max(warmup, hw);
    }
    Tracker tracker(c, warmup);
    std::ifstream idx(std::string(argv[1]) + "/frames.idx"),
        blob(std::string(argv[1]) + "/frames.bin", std::ios::binary);
    if (!idx || !blob)
      throw std::runtime_error("Missing capture");
    std::vector<Index> rows;
    std::string line;
    while (std::getline(idx, line)) {
      std::istringstream s(line);
      uint64_t number, host, id, bidx;
      Index row;
      double gain;
      if (!(s >> number >> row.camera >> row.time >> host >> row.offset >>
            row.size >> id >> bidx >> row.exposure >> gain))
        throw std::runtime_error("Malformed frame index");
      if (row.camera < 0 || row.camera > 3 || row.size != 640 * 481)
        throw std::runtime_error("Invalid frame dimensions");
      if (row.exposure > 0 && row.exposure < .0001)
        rows.push_back(row);
    }
    std::array<std::vector<Index>, 4> cameras;
    for (auto r : rows)
      cameras[r.camera].push_back(r);
    for (auto &v : cameras)
      std::sort(v.begin(), v.end(),
                [](auto a, auto b) { return a.time < b.time; });
    std::ofstream output(
        argv[7]); // argv[8] reserved below for explicit run mode
    if (std::string(argv[8]) != "--recorded")
      throw std::runtime_error("Explicit --recorded mode required");
    if (!output)
      throw std::runtime_error("Cannot open output");
    output << "#pose_reference=centroid; native Android replay; not live "
              "transport\n"
           << std::setprecision(17);
    Gyro cg, hg;
    size_t ci = 0, hi = 0;
    int solved = 0, total = 0;
    std::vector<double> durations;
    for (auto anchor : cameras[0]) {
      auto begin = std::chrono::steady_clock::now();
      double time = anchor.time * 1e-9;
      while (ci < controller.size() && controller[ci].time <= time + .004) {
        auto s = controller[ci++];
        cg.push(s.time, s.omega);
      }
      while (hi < head.size() && head[hi].time <= time + .004) {
        auto s = head[hi++];
        hg.push(s.time, s.omega);
      }
      std::vector<View> views;
      for (int camera = 0; camera < 4; camera++) {
        auto &v = cameras[camera];
        if (v.empty())
          continue;
        auto it =
            std::lower_bound(v.begin(), v.end(), anchor.time,
                             [](auto r, uint64_t t) { return r.time < t; });
        if (it == v.end())
          --it;
        else if (it != v.begin() &&
                 anchor.time - (it - 1)->time < it->time - anchor.time)
          --it;
        uint64_t lag = it->time > anchor.time ? it->time - anchor.time
                                              : anchor.time - it->time;
        if (lag > 100000)
          continue;
        cv::Mat image(481, 640, CV_8U);
        blob.seekg(it->offset);
        blob.read((char *)image.data, it->size);
        if (!blob)
          throw std::runtime_error("Truncated frame data");
        views.push_back(
            {camera, detect(image.rowRange(1, 481), c.cameras[camera])});
      }
      Pose p = tracker.update(time, views, cg, compensate ? &hg : nullptr);
      if (p.valid) {
        auto q = quaternion(p.rotation);
        output << anchor.time << ",0," << p.support << ',' << p.error
               << ",native," << p.position[0] << ',' << p.position[1] << ','
               << p.position[2] << ',' << q[0] << ',' << q[1] << ',' << q[2]
               << ',' << q[3] << '\n';
        solved++;
      } else
        output << anchor.time << ",0,0,,,,,,,,,\n";
      total++;
      durations.push_back(std::chrono::duration<double, std::milli>(
                              std::chrono::steady_clock::now() - begin)
                              .count());
    }
    std::cerr << "frames=" << total << " solved=" << solved
              << " frame_ms_p50=" << percentile(durations, .5)
              << " p99=" << percentile(durations, .99)
              << " max=" << percentile(durations, 1) << '\n';
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
  return 0;
}
