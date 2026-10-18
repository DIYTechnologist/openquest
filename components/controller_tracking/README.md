# Native Quest 1 controller tracking (experimental)

Native Android arm64 controller pose estimation, a versioned local sensor/pose socket, and
**opt-in LineageOS system-ext packaging**. This is an integration building block, not a complete
LineageOS VR stack. There is no Monado/OpenXR driver. An experimental direct camera/SyncBoss stdin bridge
is available for bounded hardware bring-up; it is not an automatically enabled ROM service. Installing it alone does not make VR applications receive controller poses.

## Verified

- Built with Android NDK 26.1.10909125 and OpenCV Android 4.12.0 on macOS, without a Linux ROM build.
- Native synthetic tests on a stock Quest 1: 64-bit timestamp parsing, gyro integration and
  dropout rejection, moving-head geometry, merged LED splitting, blank images.
- Temporary native socket service on the Quest: pose delivery, IMU-loss invalidation, reconnect
  state reset, stale and oversized message rejection. No hardware ownership or SELinux changes.
- Native replay of a separately recorded combined headset/controller-motion sequence:
  490/503 poses, all frames after initialization. Fixed earlier reference calibration on
  timestamp-matched poses gives 13.1 mm position RMSE, 25.1 mm maximum difference, 4.46-degree
  median and 7.69-degree maximum orientation difference. No fitting to that sequence.
- On-device replay cost with Meta tracking also running: median 39.0 ms, p99 58.0 ms, maximum
  69.1 ms per four-camera frame set, including file reads and detection. At 25 Hz the budget is
  40 ms, so this is **not a demonstrated sustained real-time budget**. Live queuing / latency and
  sustained performance still need validation.

The LineageOS Soong/init/SELinux integration has not been compiled in a full ROM or boot-tested.
The available device tree's previous boot failure remains a separate blocker. The initial
native port exposed a 32-bit JSON timestamp truncation bug; the dedicated uint64 parser and
regression test fix it. Missing gyro coverage now invalidates an established track instead of
allowing an unconstrained visual orientation flip.

## Build

```sh
export ANDROID_NDK_HOME=/path/to/android-ndk-r26b
./components/controller_tracking/build-android.sh
```

This downloads the checksum-pinned official OpenCV 4.12.0 Android SDK if needed. For an existing
SDK, set `OPENCV_ANDROID_SDK` to its `OpenCV-android-sdk` directory. Outputs, runtime libraries
and dependency license notices are placed in ignored `prebuilts/`; they contain no calibration.
No firmware or partitions are modified. The new component's source is MIT licensed; OpenCV and
NDK runtime libraries retain their own licenses.

For a desktop build with OpenCV 4 development libraries:

```sh
cmake -S components/controller_tracking -B /tmp/oq-controller-build
cmake --build /tmp/oq-controller-build -j2
/tmp/oq-controller-build/oq-controller-selftest
sudo ctest --test-dir /tmp/oq-controller-build --output-on-failure
```

The socket test needs root or Android system UID because those are the authorized producer UIDs.
It creates synthetic configuration in a temporary folder, launches a temporary service and stops
it afterward. Synthetic fixtures are independent of all private per-device calibration.

## LineageOS integration

Place this repository at `external/openquest` in the LineageOS source tree. Build the native
prebuilts first. In the device product, explicitly opt in:

```make
# device.mk (or the device product makefile)
ifeq ($(WITH_OPENQUEST_CONTROLLER_TRACKING),true)
$(call inherit-product, external/openquest/components/controller_tracking/lineage/product.mk)
endif
```

In the device BoardConfig:

```make
ifeq ($(WITH_OPENQUEST_CONTROLLER_TRACKING),true)
include external/openquest/components/controller_tracking/lineage/BoardConfig.mk
endif
```

Set `WITH_OPENQUEST_CONTROLLER_TRACKING := true` before inheriting those files. The service is
installed under `/system_ext/bin`; init creates `/dev/socket/openquest_controller` owned by
system:system, mode 0660. It is **disabled by default**, with no automatic camera takeover.
The included policy grants read-only private calibration access and local IPC; it grants no
camera, SyncBoss, ptrace, Meta Binder, permissive-domain or global SELinux access.

Provision these per-unit files privately, owned by system:system, under `/data/misc/openquest/`:

- `cameras.json`: converted Basalt camera calibration (`value0/T_imu_cam` and intrinsics).
- `controller-right.json`: output of `tools/controller_tracking/factory_controller_model.py`.
- `gyro.json`: measured `gyro_raw_to_rad_s` and `time_correction_s` calibration. Reading the exact
  gyro scale from controller configuration instead of measurement remains open.

Do not ship personal calibration, serials or camera recordings in the ROM/repository.
After a privileged producer is connected and the SELinux policy is validated on the target ROM,
enable with `setprop persist.openquest.controller.enabled 1`; disable with value `0`. No service
or ROM was installed to the stock headset as part of testing this contribution.

## Producer / consumer contract

`src/protocol.hpp` is the v1 little-endian stream ABI. One root/system-UID camera/runtime bridge
connects, supplies samples and receives one pose response per frame set:

1. Controller and headset gyro: factory-rectified rad/s, body/model axes, monotonic sample time.
   The producer must convert units, subtract factory bias, apply rectification, deduplicate
   samples and synchronize clocks. The raw controller scale is not guessed by the service.
2. Synchronized camera observations: four (or fewer) calibrated views, each within 100 us of the
   frame anchor; either unit bearings or raw 640x480 gray images without the metadata row.
3. Response: timestamp, validity flags, LED support, normalized reprojection error, position and
   xyzw quaternion. Unobserved/stale poses have **no position/orientation-valid flags**.

The reported transform is the **LED model centroid in headset IMU coordinates**. It is neither
an Android sensor HAL nor a world-space controller pose. A runtime must apply a fixed model to
controller grip/aim calibration, compose with headset VIO at the same timestamp, and map that
to the OpenXR tracking origin. Respect invalid flags and independently reject results older
than 100 ms; do not freeze a stale pose while claiming tracking is valid. The service also drops
validity for results already older than 100 ms when processing completes.

The producer should keep at most one pending frame set and drop superseded images; it must not
build up a delayed camera queue. Reconnecting clears all estimator state. A gyro discontinuity
requires reconnect/reinitialization. Socket messages have bounded sizes and deadlines.
`--fixed-head` is a diagnostic mode only; normal service startup requires headset gyro coverage.

## Recorded-data replay

```sh
LD_LIBRARY_PATH=/path/to/runtime-libraries ./oq-controller-replay \
  CAPTURE CAMERAS MODEL GYRO_CALIBRATION CONTROLLER_IMU HEAD_IMU OUTPUT --recorded
```

`CAPTURE` contains `frames.idx` and `frames.bin` in the existing recording format; IMU JSONL is
produced by the controller/head parsers. Passing `-` for `HEAD_IMU` means fixed-head replay.
This mode reads existing recordings, does not control cameras, and never injects poses.

## Remaining end-to-end work

- Supply a synchronized open camera/SyncBoss producer (the validated recordings were collected
  with a stock-service capture hook; that hook is not a LineageOS backend).
- Implement and test the Monado/OpenXR consumer, grip/aim origins and positional head-VIO link.
- Compile the opt-in device-tree integration, enforce SELinux on the target ROM and boot it.
- Validate live timing, sustained on-device load, faster motion, occlusion and long sessions.

See [the replay research notes](../../docs/controller-optical-prototype.md) for the earlier
recording results and limits. This contribution deliberately exposes the unfinished links
instead of declaring controller 6DoF or the whole LineageOS port complete.

## Experimental direct input

A six-second stock-firmware ownership-handoff test delivered 105 synchronized four-camera
sets to the service with both IMUs and no Meta userspace producer. It produced **zero valid
controller poses**. Input transport is demonstrated; optical live tracking is still under
validation. Camera/IMU availability must not be presented as a completed 6DoF fix.


`oq-controller-live` consumes `cam_kernel` with `CAMKERNEL_CONTROLLER_STREAM=1` and
`CAMKERNEL_PERIOD_US=40000`. The camera process emits raw SyncBoss packets and only the
short IR exposures from all four cameras. The bridge rectifies head gyro using private
factory calibration, calibrates the right controller gyro, learns the exposure/frame-counter
correspondence, and feeds the existing socket service. Its pending frame queue is limited to
one batch; superseded frames are dropped rather than building an unbounded delay.

Build the camera on macOS or Linux with `components/camera/build-android.sh`. It fetches only
seven UAPI headers from the pinned official Quest kernel, not a full kernel checkout.

Bridge arguments:

```
oq-controller-live CAMERAS MODEL GYRO_CALIBRATION HEAD_FACTORY_CALIBRATION SOCKET OUTPUT_CSV
```

Do not start the camera while stock tracking/camera/sensor services own the hardware.
Hardware bring-up currently requires a supervised, temporary ownership handoff with a
separate watchdog restoring those services. The bridge itself does not stop services or
change SELinux. No automatic camera launch or hardware SELinux policy is included.
Keep all per-unit factory calibration private.

The clock conversion uses a causal half-second receipt prefix, then freezes its offset.
Long-session drift and continuous runtime recovery are not validated. Poses remain
head-relative controller model-centroid poses; this does not provide world-space head
tracking, grip/aim transforms, or application-facing OpenXR delivery.
