// euroc_runner.cpp — run OpenVINS on an EuRoC-format dataset with no ROS.
//
// OpenVINS ships only its own simulator and ROS-based runners, so there is no way to feed it a
// plain EuRoC folder headlessly. This drives VioManager directly: it interleaves the IMU and
// stereo image streams in timestamp order and writes a TUM-format trajectory.
//
// Why OpenVINS at all: Basalt's front end tracks a patch straight from the cam0 image into the
// cam1 image, which assumes near-parallel cameras. The Quest's best pair is 19.6 deg apart, so
// that match never lands (see notes/12). OpenVINS handles arbitrary multi-camera rigs.
//
// Usage: euroc_runner <estimator_config.yaml> <dataset_dir> <out_trajectory.txt>

#include <algorithm>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include <opencv2/opencv.hpp>

#include "core/VioManager.h"
#include "core/VioManagerOptions.h"
#include "state/State.h"
#include "utils/sensor_data.h"

using namespace ov_msckf;

struct ImuRow {
  double t;
  Eigen::Vector3d w, a;
};
struct CamRow {
  double t;
  std::string file;
};

static std::vector<std::string> split(const std::string &s, char d) {
  std::vector<std::string> out;
  std::stringstream ss(s);
  std::string item;
  while (std::getline(ss, item, d)) out.push_back(item);
  return out;
}

static std::vector<ImuRow> load_imu(const std::string &p) {
  std::vector<ImuRow> v;
  std::ifstream f(p);
  std::string line;
  while (std::getline(f, line)) {
    if (line.empty() || line[0] == '#') continue;
    auto c = split(line, ',');
    if (c.size() < 7) continue;
    ImuRow r;
    r.t = std::stod(c[0]) * 1e-9;                       // EuRoC stores ns; OpenVINS wants seconds
    r.w << std::stod(c[1]), std::stod(c[2]), std::stod(c[3]);
    r.a << std::stod(c[4]), std::stod(c[5]), std::stod(c[6]);
    v.push_back(r);
  }
  return v;
}

static std::vector<CamRow> load_cam(const std::string &p) {
  std::vector<CamRow> v;
  std::ifstream f(p);
  std::string line;
  while (std::getline(f, line)) {
    if (line.empty() || line[0] == '#') continue;
    auto c = split(line, ',');
    if (c.size() < 2) continue;
    v.push_back({std::stod(c[0]) * 1e-9, c[1]});
  }
  return v;
}

int main(int argc, char **argv) {
  if (argc < 4) {
    std::cerr << "usage: euroc_runner <config.yaml> <dataset_dir> <out.txt>\n";
    return 1;
  }
  std::string config_path = argv[1], dataset = argv[2], out_path = argv[3];

  auto parser = std::make_shared<ov_core::YamlParser>(config_path);
  VioManagerOptions params;
  params.print_and_load(parser);
  params.num_opencv_threads = 1;
  auto sys = std::make_shared<VioManager>(params);

  auto imu = load_imu(dataset + "/mav0/imu0/data.csv");
  auto c0 = load_cam(dataset + "/mav0/cam0/data.csv");
  auto c1 = load_cam(dataset + "/mav0/cam1/data.csv");
  std::cout << "loaded imu=" << imu.size() << " cam0=" << c0.size() << " cam1=" << c1.size()
            << std::endl;
  if (imu.empty() || c0.empty()) return 1;

  std::ofstream out(out_path);
  out << "# timestamp tx ty tz qx qy qz qw" << std::endl;
  out << std::fixed << std::setprecision(9);

  size_t iidx = 0, n1 = 0, poses = 0;
  for (size_t k = 0; k < c0.size(); k++) {
    // feed every IMU sample up to this frame first, so the propagator has what it needs
    while (iidx < imu.size() && imu[iidx].t <= c0[k].t) {
      ov_core::ImuData m;
      m.timestamp = imu[iidx].t;
      m.wm = imu[iidx].w;
      m.am = imu[iidx].a;
      sys->feed_measurement_imu(m);
      iidx++;
    }

    cv::Mat i0 = cv::imread(dataset + "/mav0/cam0/data/" + c0[k].file, cv::IMREAD_GRAYSCALE);
    if (i0.empty()) continue;

    ov_core::CameraData cam;
    cam.timestamp = c0[k].t;
    cam.sensor_ids.push_back(0);
    cam.images.push_back(i0);
    cam.masks.push_back(cv::Mat::zeros(i0.rows, i0.cols, CV_8UC1));

    // pair with cam1 by exact timestamp; the builder guarantees identical stamps
    if (n1 < c1.size()) {
      while (n1 + 1 < c1.size() && c1[n1].t < c0[k].t - 1e-6) n1++;
      if (std::abs(c1[n1].t - c0[k].t) < 1e-6) {
        cv::Mat i1 = cv::imread(dataset + "/mav0/cam1/data/" + c1[n1].file, cv::IMREAD_GRAYSCALE);
        if (!i1.empty()) {
          cam.sensor_ids.push_back(1);
          cam.images.push_back(i1);
          cam.masks.push_back(cv::Mat::zeros(i1.rows, i1.cols, CV_8UC1));
        }
      }
    }

    sys->feed_measurement_camera(cam);

    if (sys->initialized()) {
      auto state = sys->get_state();
      Eigen::Matrix<double, 4, 1> q = state->_imu->quat();   // JPL (x,y,z,w)
      Eigen::Matrix<double, 3, 1> p = state->_imu->pos();
      out << state->_timestamp << " " << p(0) << " " << p(1) << " " << p(2) << " " << q(0) << " "
          << q(1) << " " << q(2) << " " << q(3) << std::endl;
      poses++;
    }
    if (k % 100 == 0)
      std::cout << "frame " << k << "/" << c0.size() << " initialized=" << sys->initialized()
                << " poses=" << poses << std::endl;
  }
  std::cout << "done: wrote " << poses << " poses to " << out_path << std::endl;
  return 0;
}
