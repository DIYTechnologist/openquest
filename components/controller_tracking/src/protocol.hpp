// SPDX-License-Identifier: MIT
#pragma once
#include <cstddef>
#include <cstdint>
// Version 1 is little-endian IEEE-754, as on Android arm64. Lengths exclude
// Header. Sensor timestamps are exposure/sample CLOCK_MONOTONIC nanoseconds,
// not receipts.
namespace oq::wire {
constexpr uint32_t magic = 0x3154514f; // OQT1
constexpr uint16_t version = 1;
constexpr uint32_t maximum_payload = 4 * (640 * 480 + 16) + 4;
enum Type : uint16_t {
  ControllerGyro = 1,
  HeadGyro = 2,
  Bearings = 3,
  Images = 4,
  Result = 0x8001
};
enum Flag : uint32_t {
  PositionValid = 1,
  OrientationValid = 2,
  HeadRelative = 4
};
struct Header {
  uint32_t magic;
  uint16_t version, type;
  uint32_t bytes, reserved;
  uint64_t timestamp_ns;
};
struct Pose {
  uint64_t timestamp_ns;
  uint32_t flags, support;
  double position[3], quaternion_xyzw[4], error;
};
static_assert(sizeof(Header) == 24);
static_assert(sizeof(Pose) == 80);
// Bearing payload: u32 view count, then {u32 camera,u32 ray_count,u64
// exposure_ns, f64 unit_ray[ray_count][3]} per view. Max 4 views / 256 rays per
// view. Image payload: u32 view count, then {u32 camera,u16 width,u16
// height,u64 exposure_ns, u8 pixels[width*height]} per view. Exactly 640x480,
// no metadata row / stride. Gyro payload: f64 omega[3], factory-rectified rad/s
// in that body's model axes.
} // namespace oq::wire
