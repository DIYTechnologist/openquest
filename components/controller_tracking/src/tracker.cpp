// SPDX-License-Identifier: MIT
#include "tracker.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
#include <opencv2/calib3d.hpp>
#include <opencv2/imgproc.hpp>

#include <set>
#include <stdexcept>
#include <tuple>

namespace oq {
static R exp(V v) {
  R r;
  cv::Rodrigues(v, r);
  return r;
}
static V log(R r) {
  V v;
  cv::Rodrigues(r, v);
  return v;
}
static double angle(R a, R b) {
  return std::acos(std::clamp((cv::trace(a.t() * b) - 1) * .5, -1., 1.));
}
static bool finite(V v) {
  return std::isfinite(v[0]) && std::isfinite(v[1]) && std::isfinite(v[2]);
}
static V point(cv::FileNode n) {
  if (n.size() != 3)
    throw std::runtime_error("Expected three-vector");
  return V((double)n[0], (double)n[1], (double)n[2]);
}
static R matrix(cv::FileNode n) {
  if (n.size() != 9)
    throw std::runtime_error("Expected 3x3 matrix");
  R r;
  for (int i = 0; i < 9; i++)
    r.val[i] = (double)n[i];
  return r;
}
Config load_config(const char *cameras, const char *model,
                   const char *calibration) {
  cv::FileStorage c(cameras, cv::FileStorage::READ),
      m(model, cv::FileStorage::READ), g(calibration, cv::FileStorage::READ);
  if (!c.isOpened() || !m.isOpened() || !g.isOpened())
    throw std::runtime_error("Cannot open calibration");
  Config out;
  V center(0, 0, 0);
  for (auto n : m["points_imu_frame_at_capture"]) {
    V p = point(n);
    if (!finite(p) || cv::norm(p) > .5)
      throw std::runtime_error("Invalid LED position");
    out.model.push_back(p);
    center += p;
  }
  if (out.model.size() < 4 || out.model.size() > 32)
    throw std::runtime_error("Invalid LED count");
  center *= 1. / out.model.size();
  for (auto &p : out.model)
    p -= center;
  for (auto n : m["led_normals"]) {
    V v = point(n);
    if (!finite(v) || std::abs(cv::norm(v) - 1) > .1)
      throw std::runtime_error("Invalid LED normal");
    out.normals.push_back(v / cv::norm(v));
  }
  if (out.normals.size() != out.model.size())
    throw std::runtime_error("LED normal count mismatch");
  out.gyro_matrix = matrix(m["gyro_rectification"]);
  out.gyro_bias = point(m["gyro_offset_si"]);
  out.gyro_scale = (double)g["gyro_raw_to_rad_s"];
  out.gyro_time_correction = (double)g["time_correction_s"];
  if (!(out.gyro_scale > 0 && out.gyro_scale < 1))
    throw std::runtime_error("Invalid gyro scale");
  auto root = c["value0"];
  for (int i = 0; i < 4; i++) {
    auto t = root["T_imu_cam"][i];
    cv::Vec4d q((double)t["qx"], (double)t["qy"], (double)t["qz"],
                (double)t["qw"]);
    double length = cv::norm(q);
    if (!std::isfinite(length) || length < .9 || length > 1.1)
      throw std::runtime_error("Invalid camera quaternion");
    q *= 1 / length;
    double x = q[0], y = q[1], z = q[2], w = q[3];
    out.cameras[i].rotation =
        R(1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w),
          2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w),
          2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y));
    out.cameras[i].origin =
        V((double)t["px"], (double)t["py"], (double)t["pz"]);
    auto intr = root["intrinsics"][i]["intrinsics"];
    const char *keys[] = {"fx", "fy", "cx", "cy", "k1", "k2", "k3", "k4"};
    for (int j = 0; j < 8; j++) {
      out.cameras[i].intrinsics[j] = (double)intr[keys[j]];
      if (!std::isfinite(out.cameras[i].intrinsics[j]))
        throw std::runtime_error("Invalid intrinsics");
    }
    if (out.cameras[i].intrinsics[0] <= 0 || out.cameras[i].intrinsics[1] <= 0)
      throw std::runtime_error("Invalid focal length");
  }
  return out;
}
void Gyro::push(double t, V w) {
  if (!std::isfinite(t) || !finite(w) ||
      (!samples.empty() && t <= samples.back().time))
    throw std::runtime_error("Nonmonotonic/nonfinite gyro");
  samples.push_back({t, w});
  while (samples.size() > 12000)
    samples.pop_front();
}
bool Gyro::delta(double start, double end, R &out) const {
  if (samples.size() < 2 || end <= start || start < samples.front().time ||
      end > samples.back().time)
    return false;
  auto it = std::lower_bound(samples.begin(), samples.end(), start,
                             [](auto a, double t) { return a.time < t; });
  if (it == samples.begin())
    ++it;
  auto interpolate = [](Sample a, Sample b, double t) {
    double f = (t - a.time) / (b.time - a.time);
    return a.omega * (1 - f) + b.omega * f;
  };
  V w = interpolate(*(it - 1), *it, start);
  double t = start;
  out = R::eye();
  for (; it != samples.end() && t < end; ++it) {
    double next = std::min(it->time, end);
    if (next <= t)
      continue;
    if (next - t > .01)
      return false;
    V nw = interpolate(*(it - 1), *it, next);
    out = out * exp((w + nw) * (.5 * (next - t)));
    w = nw;
    t = next;
  }
  return t >= end;
}
std::vector<V> detect(const cv::Mat &image, const Camera &camera) {
  if (image.type() != CV_8UC1 || image.rows != 480 || image.cols != 640)
    throw std::runtime_error("Expected 640x480 gray image");
  std::vector<cv::Point2d> centers;
  for (int threshold : {40, 128}) {
    cv::Mat mask, labels, stats, centroids;
    cv::threshold(image, mask, threshold, 255, cv::THRESH_BINARY);
    int count = cv::connectedComponentsWithStats(mask, labels, stats, centroids,
                                                 8, CV_32S);
    for (int i = 1; i < count; i++) {
      int area = stats.at<int>(i, cv::CC_STAT_AREA);
      if (area < 1 || area > 200)
        continue;
      int x0 = stats.at<int>(i, cv::CC_STAT_LEFT),
          y0 = stats.at<int>(i, cv::CC_STAT_TOP),
          width = stats.at<int>(i, cv::CC_STAT_WIDTH),
          height = stats.at<int>(i, cv::CC_STAT_HEIGHT);
      double total = 0, xsum = 0, ysum = 0;
      int peak = 0;
      for (int y = y0; y < y0 + height; y++)
        for (int x = x0; x < x0 + width; x++)
          if (labels.at<int>(y, x) == i) {
            int v = image.at<uint8_t>(y, x);
            total += v;
            xsum += v * x;
            ysum += v * y;
            peak = std::max(peak, v);
          }
      if (total <= 0 || peak < 80 || (threshold == 40 && peak > 128))
        continue;
      centers.emplace_back(xsum / total, ysum / total);
    }
  }
  std::vector<V> rays;
  auto k = camera.intrinsics;
  for (auto p : centers) {
    double x = (p.x - k[2]) / k[0], y = (p.y - k[3]) / k[1],
           rd = std::hypot(x, y);
    if (rd < 1e-9) {
      rays.emplace_back(0, 0, 1);
      continue;
    }
    double th = rd;
    for (int i = 0; i < 20; i++) {
      double s = th * th;
      double f = th * (1 + k[4] * s + k[5] * s * s + k[6] * s * s * s +
                       k[7] * s * s * s * s) -
                 rd;
      double df = 1 + 3 * k[4] * s + 5 * k[5] * s * s + 7 * k[6] * s * s * s +
                  9 * k[7] * s * s * s * s;
      th -= f / df;
    }
    V ray(std::sin(th) * x / rd, std::sin(th) * y / rd, std::cos(th));
    if (finite(ray))
      rays.push_back(ray);
  }
  return rays;
}
struct Match {
  int view, model, ray;
};
static std::vector<Match> associate(const Config &c, const Pose &p,
                                    const std::vector<View> &views,
                                    double gate) {
  std::vector<Match> matches;
  for (int v = 0; v < (int)views.size(); v++) {
    auto cam = c.cameras[views[v].camera];
    std::vector<std::tuple<double, int, int>> candidates;
    for (int i = 0; i < (int)c.model.size(); i++) {
      V world = p.rotation * c.model[i] + p.position,
        normal = p.rotation * c.normals[i], direction = cam.origin - world;
      V camera = cam.rotation.t() * (world - cam.origin);
      if (camera[2] <= 1e-9 ||
          direction.dot(normal) < -.0872 * cv::norm(direction))
        continue;
      for (int j = 0; j < (int)views[v].rays.size(); j++) {
        V ray = views[v].rays[j];
        if (ray[2] <= 1e-9)
          continue;
        double error = std::hypot(camera[0] / camera[2] - ray[0] / ray[2],
                                  camera[1] / camera[2] - ray[1] / ray[2]);
        if (error <= gate)
          candidates.emplace_back(error, i, j);
      }
    }
    std::sort(candidates.begin(), candidates.end());
    std::set<int> used_model, used_ray;
    for (auto [e, i, j] : candidates)
      if (!used_model.count(i) && !used_ray.count(j)) {
        used_model.insert(i);
        used_ray.insert(j);
        matches.push_back({v, i, j});
      }
  }
  return matches;
}
static int support(const std::vector<Match> &m) {
  std::set<int> ids;
  for (auto x : m)
    ids.insert(x.model);
  return ids.size();
}
static bool translation(const Config &c, Pose &p,
                        const std::vector<View> &views,
                        const std::vector<Match> &matches) {
  cv::Mat A = cv::Mat::zeros(matches.size() * 3, 3, CV_64F),
          b = cv::Mat::zeros(matches.size() * 3, 1, CV_64F);
  for (int i = 0; i < (int)matches.size(); i++) {
    auto m = matches[i];
    auto cam = c.cameras[views[m.view].camera];
    V ray = cam.rotation * views[m.view].rays[m.ray];
    ray /= cv::norm(ray);
    R P = R::eye() - ray * ray.t();
    V rhs = P * (cam.origin - p.rotation * c.model[m.model]);
    for (int y = 0; y < 3; y++) {
      b.at<double>(3 * i + y) = rhs[y];
      for (int x = 0; x < 3; x++)
        A.at<double>(3 * i + y, x) = P(y, x);
    }
  }
  cv::Mat singular;
  cv::SVD::compute(A, singular);
  if (singular.rows < 3 || singular.at<double>(2) <= 0 ||
      singular.at<double>(0) / singular.at<double>(2) > 2000)
    return false;
  cv::Mat t;
  cv::solve(A, b, t, cv::DECOMP_SVD);
  p.position = V(t);
  return finite(p.position);
}
static bool residual(const Config &c, const Pose &p,
                     const std::vector<View> &views,
                     const std::vector<Match> &matches, cv::Mat &r,
                     cv::Mat &J) {
  r.create(matches.size() * 2, 1, CV_64F);
  J.create(matches.size() * 2, 6, CV_64F);
  for (int i = 0; i < (int)matches.size(); i++) {
    auto m = matches[i];
    auto cam = c.cameras[views[m.view].camera];
    V a = p.rotation * c.model[m.model],
      q = cam.rotation.t() * (a + p.position - cam.origin),
      obs = views[m.view].rays[m.ray];
    if (q[2] <= 1e-9 || obs[2] <= 1e-9)
      return false;
    cv::Matx<double, 2, 3> projection(1 / q[2], 0, -q[0] / (q[2] * q[2]), 0,
                                      1 / q[2], -q[1] / (q[2] * q[2]));
    auto D = projection * cam.rotation.t();
    R negative_skew(0, a[2], -a[1], -a[2], 0, a[0], a[1], -a[0], 0);
    auto rot = D * negative_skew;
    for (int y = 0; y < 2; y++) {
      r.at<double>(2 * i + y) = q[y] / q[2] - obs[y] / obs[2];
      for (int x = 0; x < 3; x++) {
        J.at<double>(2 * i + y, x) = rot(y, x);
        J.at<double>(2 * i + y, x + 3) = D(y, x);
      }
    }
  }
  return true;
}
static bool refine(const Config &c, Pose &p, R predicted,
                   const std::vector<View> &views) {
  std::vector<Match> matches;
  for (double gate : {.03, .015, .006}) {
    matches = associate(c, p, views, gate);
    if (support(matches) < 3 || !translation(c, p, views, matches))
      return false;
    for (int it = 0; it < 4; it++) {
      cv::Mat r, J;
      if (!residual(c, p, views, matches, r, J))
        return false;
      V prior = log(p.rotation * predicted.t());
      if (cv::norm(prior) > .2)
        return false;
      cv::Mat A = cv::Mat::zeros(r.rows + 3, 6, CV_64F),
              b = cv::Mat::zeros(r.rows + 3, 1, CV_64F);
      for (int i = 0; i < r.rows; i++) {
        double weight = std::sqrt(
            std::min(1., .005 / std::max(std::abs(r.at<double>(i)), 1e-12)));
        for (int j = 0; j < 6; j++)
          A.at<double>(i, j) = J.at<double>(i, j) * weight;
        b.at<double>(i) = -r.at<double>(i) * weight;
      }
      for (int i = 0; i < 3; i++) {
        A.at<double>(r.rows + i, i) = .2;
        b.at<double>(r.rows + i) = -.2 * prior[i];
      }
      cv::Mat step;
      cv::solve(A, b, step, cv::DECOMP_SVD);
      if (!cv::checkRange(step) || cv::norm(step) > .5)
        return false;
      p.rotation =
          exp(V(step.at<double>(0), step.at<double>(1), step.at<double>(2))) *
          p.rotation;
      p.position +=
          V(step.at<double>(3), step.at<double>(4), step.at<double>(5));
      if (cv::norm(step) < 1e-7)
        break;
    }
  }
  cv::Mat r, J;
  if (!residual(c, p, views, matches, r, J))
    return false;
  double error = 0;
  for (int i = 0; i < r.rows; i += 2)
    error += std::hypot(r.at<double>(i), r.at<double>(i + 1));
  error /= matches.size();
  if (error > .006)
    return false;
  p.error = error;
  p.support = support(matches);
  p.valid = true;
  return true;
}
struct CloudPoint {
  V point;
  int a, b;
};
static std::vector<CloudPoint> cloud(const Config &c, const View &a,
                                     const View &b) {
  auto A = c.cameras[a.camera], B = c.cameras[b.camera];
  std::vector<CloudPoint> out;
  V baseline = B.origin - A.origin;
  for (int i = 0; i < (int)a.rays.size(); i++)
    for (int j = 0; j < (int)b.rays.size(); j++) {
      V da = A.rotation * a.rays[i], db = B.rotation * b.rays[j],
        plane = da.cross(baseline);
      double pn = cv::norm(plane);
      if (pn > 1e-9 && std::abs(db.dot(plane / pn)) > .005)
        continue;
      V n = da.cross(db);
      double nn = n.dot(n);
      if (nn < 1e-12)
        continue;
      double ta = baseline.dot(db.cross(n)) / nn,
             tb = baseline.dot(da.cross(n)) / nn;
      if (ta <= 0 || tb <= 0)
        continue;
      V pa = A.origin + da * ta, pb = B.origin + db * tb;
      if (cv::norm(pa - pb) > .005)
        continue;
      out.push_back({(pa + pb) * .5, i, j});
    }
  return out;
}
struct MetricMatch {
  int model, cloud;
};
static std::vector<MetricMatch>
metric_matches(const Config &c, const Pose &p,
               const std::vector<CloudPoint> &points, const View &a,
               const View &b) {
  std::vector<std::tuple<double, int, int>> all;
  auto A = c.cameras[a.camera], B = c.cameras[b.camera];
  for (int i = 0; i < (int)c.model.size(); i++) {
    V world = p.rotation * c.model[i] + p.position,
      normal = p.rotation * c.normals[i];
    V ra = A.origin - world, rb = B.origin - world;
    if (ra.dot(normal) < -.0872 * cv::norm(ra) ||
        rb.dot(normal) < -.0872 * cv::norm(rb))
      continue;
    for (int j = 0; j < (int)points.size(); j++) {
      double d = cv::norm(world - points[j].point);
      if (d <= .012)
        all.emplace_back(d, i, j);
    }
  }
  std::sort(all.begin(), all.end());
  std::set<int> mi, ai, bi;
  std::vector<MetricMatch> matches;
  for (auto [d, i, j] : all)
    if (!mi.count(i) && !ai.count(points[j].a) && !bi.count(points[j].b)) {
      mi.insert(i);
      ai.insert(points[j].a);
      bi.insert(points[j].b);
      matches.push_back({i, j});
    }
  return matches;
}
static Pose rigid(const Config &c, const std::vector<CloudPoint> &cloud,
                  const std::vector<MetricMatch> &matches) {
  V a(0, 0, 0), b(0, 0, 0);
  for (auto m : matches) {
    a += c.model[m.model];
    b += cloud[m.cloud].point;
  }
  a *= 1. / matches.size();
  b *= 1. / matches.size();
  R H = R::zeros();
  for (auto m : matches)
    H += (c.model[m.model] - a) * (cloud[m.cloud].point - b).t();
  cv::SVD svd(cv::Mat(H), cv::SVD::FULL_UV);
  R u(svd.u), vt(svd.vt), D = R::eye();
  D(2, 2) = cv::determinant(vt.t() * u.t());
  Pose p;
  p.rotation = vt.t() * D * u.t();
  p.position = b - p.rotation * a;
  return p;
}
static bool joint(const Config &c, Pose &p, const std::vector<View> &views,
                  const std::vector<Match> &matches) {
  for (int it = 0; it < 8; it++) {
    cv::Mat r, J;
    if (!residual(c, p, views, matches, r, J))
      return false;
    cv::Mat weights(r.size(), CV_64F);
    for (int i = 0; i < r.rows; i++)
      weights.at<double>(i) = std::sqrt(
          std::min(1., .005 / std::max(std::abs(r.at<double>(i)), 1e-12)));
    cv::Mat A = J.clone(), b = r.clone();
    for (int i = 0; i < r.rows; i++) {
      A.row(i) *= weights.at<double>(i);
      b.at<double>(i) *= -weights.at<double>(i);
    }
    cv::Mat step;
    cv::solve(A, b, step, cv::DECOMP_SVD);
    if (!cv::checkRange(step) || cv::norm(step) > 1)
      return false;
    bool accepted = false;
    for (double alpha : {1., .5, .25, .125}) {
      Pose next = p;
      next.rotation =
          exp(V(step.at<double>(0), step.at<double>(1), step.at<double>(2)) *
              alpha) *
          p.rotation;
      next.position +=
          V(step.at<double>(3), step.at<double>(4), step.at<double>(5)) * alpha;
      cv::Mat nr, nJ;
      if (residual(c, next, views, matches, nr, nJ) &&
          cv::norm(nr.mul(weights)) <= cv::norm(r.mul(weights))) {
        p = next;
        accepted = true;
        break;
      }
    }
    if (!accepted || cv::norm(step) < 1e-8)
      break;
  }
  return true;
}
static Pose register_pair(const Config &c, const View &a, const View &b,
                          const R *predicted) {
  auto points = cloud(c, a, b);
  Pose best;
  if (points.size() < 3 || points.size() > 80)
    return best;
  struct Triangle {
    int a, b, c;
    V distances;
  };
  std::vector<Triangle> triangles;
  for (int i = 0; i < (int)c.model.size(); i++)
    for (int j = 0; j < (int)c.model.size(); j++)
      for (int k = 0; k < (int)c.model.size(); k++)
        if (i != j && i != k && j != k)
          triangles.push_back({i, j, k,
                               V(cv::norm(c.model[i] - c.model[j]),
                                 cv::norm(c.model[i] - c.model[k]),
                                 cv::norm(c.model[j] - c.model[k]))});
  struct Candidate {
    Pose pose;
    std::vector<MetricMatch> matches;
    double error;
  };
  std::vector<Candidate> shortlist;
  for (int i = 0; i < (int)points.size(); i++)
    for (int j = i + 1; j < (int)points.size(); j++)
      for (int k = j + 1; k < (int)points.size(); k++) {
        if (points[i].a == points[j].a || points[i].a == points[k].a ||
            points[j].a == points[k].a || points[i].b == points[j].b ||
            points[i].b == points[k].b || points[j].b == points[k].b)
          continue;
        V x = points[i].point, y = points[j].point, z = points[k].point;
        if (cv::norm((y - x).cross(z - x)) < 1e-5)
          continue;
        V d(cv::norm(x - y), cv::norm(x - z), cv::norm(y - z));
        for (auto triangle : triangles) {
          V difference = triangle.distances - d;
          if (std::max({std::abs(difference[0]), std::abs(difference[1]),
                        std::abs(difference[2])}) > .004)
            continue;
          auto p = rigid(c, points,
                         {{triangle.a, i}, {triangle.b, j}, {triangle.c, k}});
          if (predicted && angle(*predicted, p.rotation) > .25)
            continue;
          auto matches = metric_matches(c, p, points, a, b);
          if (matches.size() < 4)
            continue;
          for (int q = 0; q < 2; q++) {
            p = rigid(c, points, matches);
            matches = metric_matches(c, p, points, a, b);
            if (matches.size() < 4)
              break;
          }
          if (matches.size() < 4)
            continue;
          double error = 0;
          for (auto m : matches)
            error += cv::norm(p.rotation * c.model[m.model] + p.position -
                              points[m.cloud].point);
          error /= matches.size();
          bool duplicate = false;
          for (auto &old : shortlist)
            if (cv::norm(old.pose.position - p.position) < .002 &&
                angle(old.pose.rotation, p.rotation) < .08) {
              duplicate = true;
              break;
            }
          if (duplicate)
            continue;
          shortlist.push_back({p, matches, error});
          std::sort(shortlist.begin(), shortlist.end(), [](auto &x, auto &y) {
            if (x.matches.size() != y.matches.size())
              return x.matches.size() > y.matches.size();
            return x.error < y.error;
          });
          if (shortlist.size() > 64)
            shortlist.pop_back();
        }
      }
  double best_score = std::numeric_limits<double>::infinity();
  std::vector<View> views{a, b};
  for (auto candidate : shortlist) {
    auto p = candidate.pose;
    std::vector<Match> matches;
    for (auto m : candidate.matches) {
      matches.push_back({0, m.model, points[m.cloud].a});
      matches.push_back({1, m.model, points[m.cloud].b});
    }
    if (!joint(c, p, views, matches) ||
        (predicted && angle(*predicted, p.rotation) > .25))
      continue;
    auto support_matches = associate(c, p, views, .006);
    int ca = 0, cb = 0;
    double error = 0;
    for (auto m : support_matches) {
      auto cam = c.cameras[views[m.view].camera];
      V q = cam.rotation.t() *
            (p.rotation * c.model[m.model] + p.position - cam.origin),
        obs = views[m.view].rays[m.ray];
      error += std::hypot(q[0] / q[2] - obs[0] / obs[2],
                          q[1] / q[2] - obs[1] / obs[2]);
      (m.view == 0 ? ca : cb)++;
    }
    int count = std::min(ca, cb);
    if (count < 4)
      continue;
    error /= support_matches.size();
    double score = predicted ? error + .03 * angle(*predicted, p.rotation)
                             : -count + error;
    if (score < best_score) {
      best_score = score;
      best = p;
      best.valid = true;
      best.support = count;
      best.error = error;
    }
  }
  return best;
}
Tracker::Tracker(Config c, double ready_after) : config(std::move(c)) {
  if (ready_after >= 0)
    start_time = ready_after - .5;
}
Pose Tracker::update(double time, const std::vector<View> &views,
                     const Gyro &gyro, const Gyro *head) {
  if (!std::isfinite(time) || (state_time && time <= state_time))
    throw std::runtime_error("Nonmonotonic frame");
  if (start_time < 0)
    start_time = time;
  for (auto view : views)
    if (view.camera < 0 || view.camera > 3 || view.rays.size() > 256)
      throw std::runtime_error("Invalid view");
  R predicted, delta, head_delta;
  bool have_prediction =
      have_orientation && gyro.delta(state_time, time, delta);
  if (have_prediction) {
    predicted = orientation * delta;
    if (head) {
      if (head->delta(state_time, time, head_delta))
        predicted = head_delta.t() * predicted;
      else
        have_prediction = false;
    }
  }
  Pose p;
  if (have_prediction && last.valid && time - vision_time <= .5) {
    p = last;
    p.rotation = predicted;
    p.position = last.position + velocity * std::min(time - vision_time, .12);
    if (head && head->delta(vision_time, time, head_delta))
      p.position = head_delta.t() * p.position;
    p.valid = refine(config, p, predicted, views) &&
              cv::norm(p.position - last.position) <= 3 * (time - vision_time);
  }
  if (!p.valid && time - start_time >= .5 &&
      (!have_orientation || have_prediction)) {
    const int pairs[][2] = {{0, 2}, {1, 3}, {0, 3}, {1, 2}, {0, 1}, {2, 3}};
    for (auto pair : pairs) {
      const View *a = nullptr, *b = nullptr;
      for (auto &v : views) {
        if (v.camera == pair[0])
          a = &v;
        if (v.camera == pair[1])
          b = &v;
      }
      if (a && b) {
        p = register_pair(config, *a, *b,
                          have_prediction ? &predicted : nullptr);
        if (p.valid)
          break;
      }
    }
  }
  if (p.valid) {
    if (last.valid && time - vision_time > 0 && time - vision_time < .12) {
      R transport = R::eye();
      if (head && !head->delta(vision_time, time, transport))
        transport = R::eye();
      V measured =
          (p.position - transport.t() * last.position) / (time - vision_time);
      velocity = .5 * (transport.t() * velocity) + .5 * measured;
      double speed = cv::norm(velocity);
      if (speed > 2)
        velocity *= 2 / speed;
    } else
      velocity = V(0, 0, 0);
    last = p;
    orientation = p.rotation;
    vision_time = time;
    state_time = time;
    have_orientation = true;
  } else if (have_prediction) {
    orientation = predicted;
    state_time = time;
  }
  return p;
}
} // namespace oq
