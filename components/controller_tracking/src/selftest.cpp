// SPDX-License-Identifier: MIT
#include "json_integer.hpp"
#include "protocol.hpp"
#include "tracker.hpp"
#include <cmath>
#include <iostream>
#include <opencv2/calib3d.hpp>
#include <stdexcept>
#include <string>
using namespace oq;
static void require(bool value, const char *message) {
  if (!value)
    throw std::runtime_error(message);
}
static R rotation(V w) {
  R r;
  cv::Rodrigues(w, r);
  return r;
}
static Config synthetic() {
  Config c;
  c.model = {{-.04, -.03, 0},   {.04, -.03, .005}, {.04, .03, 0},
             {-.04, .03, .005}, {0, .05, -.01},    {.05, 0, .01},
             {-.02, .01, .012}};
  V center(0, 0, 0);
  for (auto p : c.model)
    center += p;
  center *= 1. / c.model.size();
  for (auto &p : c.model)
    p -= center;
  c.normals.assign(c.model.size(), V(0, 0, -1));
  for (int i = 0; i < 4; i++) {
    c.cameras[i].rotation = R::eye();
    c.cameras[i].origin = V((i - 1.5) * .05, 0, 0);
    c.cameras[i].intrinsics = {300, 300, 320, 240, 0, 0, 0, 0};
  }
  c.gyro_matrix = R::eye();
  c.gyro_bias = V(0, 0, 0);
  c.gyro_scale = .002;
  return c;
}
int main() {
  try {
    require(oq_json_timestamp("{\"t\":152336302348908}", "t") ==
                152336302348908ULL,
            "64-bit monotonic timestamp truncated");
    require(oq_json_timestamp("{\"t\":18446744073709551615}", "t") ==
                UINT64_MAX,
            "Maximum uint64 truncated");
    for (auto text :
         {"{\"t\":-1}", "{\"t\":18446744073709551616}", "{\"t\":1.5}"}) {
      bool rejected = false;
      try {
        oq_json_timestamp(text, "t");
      } catch (...) {
        rejected = true;
      }
      require(rejected, "Malformed timestamp accepted");
    }
    Gyro gyro;
    for (int i = 0; i <= 100; i++)
      gyro.push(10 + i * .002, V(0, 0, 1));
    R delta;
    require(gyro.delta(10, 10.2, delta), "Valid gyro interval rejected");
    require(cv::norm(cv::Mat(delta - rotation(V(0, 0, .2)))) < 1e-8,
            "Gyro integration incorrect");
    require(!gyro.delta(9.9, 10.1, delta), "Unobserved gyro prefix invented");
    Gyro gap;
    gap.push(10, V(0, 0, 0));
    gap.push(10.1, V(0, 0, 0));
    require(!gap.delta(10, 10.1, delta), "IMU dropout accepted");
    auto c = synthetic();
    Tracker tracker(c);
    Gyro controller, head;
    int solved = 0;
    double sample_time = 1000;
    const V omega(0, .25, 0);
    for (int frame = 0; frame < 35; frame++) {
      double time = 1000 + frame * .04;
      while (sample_time <= time + .004) {
        controller.push(sample_time, V(0, 0, 0));
        head.push(sample_time, omega);
        sample_time += .002;
      }
      R expected = rotation(-omega * (time - 1000));
      V position = expected * V(.02, -.01, .7);
      std::vector<View> views;
      for (int camera = 0; camera < 4; camera++) {
        View view{camera, {}};
        for (auto m : c.model) {
          V ray = c.cameras[camera].rotation.t() *
                  (expected * m + position - c.cameras[camera].origin);
          view.rays.push_back(ray / cv::norm(ray));
        }
        views.push_back(view);
      }
      auto pose = tracker.update(time, views, controller, &head);
      if (pose.valid) {
        solved++;
        require(cv::norm(pose.position - position) < 1e-5,
                "Synthetic position inaccurate");
        require(cv::norm(cv::Mat(pose.rotation - expected)) < 1e-4,
                "Head compensation incorrect");
      }
    }
    require(solved >= 20, "Synthetic tracking coverage failed");
    cv::Mat image = cv::Mat::zeros(480, 640, CV_8U);
    for (int y = 220; y < 260; y++)
      for (int x = 290; x < 345; x++) {
        double value =
            255 *
                std::exp(-((x - 310.) * (x - 310.) + (y - 240.) * (y - 240.)) /
                         18) +
            255 *
                std::exp(-((x - 322.) * (x - 322.) + (y - 240.) * (y - 240.)) /
                         18);
        image.at<uint8_t>(y, x) = std::min(255., value);
      }
    require(detect(image, c.cameras[0]).size() == 2,
            "Merged bright LEDs not split");
    require(detect(cv::Mat::zeros(480, 640, CV_8U), c.cameras[0]).empty(),
            "Blank image invented LEDs");
    std::cout << "PASS: uint64 timestamps, malformed input, gyro "
                 "integration/dropout, moving-head geometry ("
              << solved << " poses), split LED cores, blank image\n";
  } catch (const std::exception &e) {
    std::cerr << "FAIL: " << e.what() << '\n';
    return 1;
  }
  return 0;
}
