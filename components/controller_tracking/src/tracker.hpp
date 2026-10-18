// SPDX-License-Identifier: MIT
#pragma once
#include <array>
#include <cstdint>
#include <deque>
#include <opencv2/core.hpp>
#include <vector>

namespace oq {
using V = cv::Vec3d;
using R = cv::Matx33d;
struct Camera {
  R rotation;
  V origin;
  std::array<double, 8> intrinsics;
};
struct Config {
  std::vector<V> model, normals;
  std::array<Camera, 4> cameras;
  R gyro_matrix;
  V gyro_bias;
  double gyro_scale, gyro_time_correction;
};
struct View {
  int camera;
  std::vector<V> rays;
};
struct Pose {
  R rotation = R::eye();
  V position{0, 0, 0};
  double error = 0;
  int support = 0;
  bool valid = false;
};
struct Sample {
  double time;
  V omega;
};
class Gyro {
public:
  void push(double time, V omega);
  bool delta(double start, double end, R &out) const;

private:
  std::deque<Sample> samples;
};
Config load_config(const char *cameras, const char *model,
                   const char *gyro_calibration);
std::vector<V> detect(const cv::Mat &image, const Camera &camera);
class Tracker {
public:
  explicit Tracker(Config config, double ready_after = -1);
  Pose update(double time, const std::vector<View> &views,
              const Gyro &controller, const Gyro *head = nullptr);

private:
  Config config;
  Pose last;
  V velocity{0, 0, 0};
  R orientation = R::eye();
  double state_time = 0, vision_time = 0, start_time = -1;
  bool have_orientation = false;
};
} // namespace oq
