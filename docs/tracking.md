# `components/tracking`

Runs OpenVINS on `components/camera`'s live output and injects the resulting poses into Meta's own
compositor at frame rate. Replaces `oculus.internal.tracking.ITrackingService` (`trackingservice`).

## Status

Core done: `dumpsys tracking` reports our poses with `Valid: Yes`, `Tracking Level: 6DOF`
(`research-notes/23`); zero crashes over a 10-minute, ~18,000-pose session (`research-notes/26`);
real-time budget met, 31.75 ms/frame on-device vs a 33.3 ms budget (`research-notes/20`); accuracy
validated against Meta's own tracker as ground truth — **7.6 cm ATE in-place** and **11.7 cm ATE
over a 60.6 m room-scale walk**, both Sim3/Umeyama, with room-scale scale converging to 1.011
(`research-notes/51`, `research-notes/53`). **Open:** motion-to-photon latency, not yet measured
against the "within 2× of stock" criterion (`research-notes/18` step 4).

## What it does

- `vio_live` is the live daemon: reads `cam_kernel`'s interleaved frame+IMU stream over a pipe, runs
  OpenVINS (static initialisation only on-device — see the init-mode note below), and injects each
  resulting pose via `oculus.internal.virtual_input.ITrackingDataInjectionService` using a raw
  `AIBinder` transaction (no Meta client library).
  ```
  cam_kernel 4 <secs> 3000 160 02 -  |  vio_live <config.yaml> <imu_rect.txt> [--no-inject]
  ```
- `pose_inject` is a standalone test/replay harness for the same injection interface — replays a
  TUM-format trajectory or a synthetic circle, independent of the estimator, for isolating whether a
  problem is in injection or in tracking (`research-notes/23`).
- `replay_feed` replays a recorded `cam_kernel` capture as if it were live, so `vio_live` can be
  validated against real motion without anyone wearing the headset — OpenVINS gates init on IMU
  excitation, so a stationary desk capture never initialises (`research-notes/26`).
- `ov_bench` measures OpenVINS' per-frame cost on the Quest's own Snapdragon 835 (`research-notes/20`).

OpenVINS itself is not vendored as source in this repo: `build_ov_objs.sh` compiles it from the
`quest-openvins-android-deps` container image (OpenCV Android SDK + Boost/Eigen headers + OpenVINS
source, all baked in at image-build time), with per-file object caching so an edit to one of
`vio_live.cpp`/`pose_inject.c`/`replay_feed.c` doesn't recompile the ~200 OpenVINS sources.

**On-device init mode is static-only**, not a limitation but a considered choice:
`ov_init/src/ceres/*` and `DynamicInitializer.cpp` are excluded from the on-device build (Ceres
would be a substantial extra cross-compile port) and `dyninit_stub.cpp` keeps the link honest if
that path is ever reached. Dynamic init exists and is sometimes *necessary* (`research-notes/53`:
a capture with no well-textured stationary window has no other way to initialise correctly) but
that discovery was made in the host-side validation pipeline (`tools/vio/make_openvins_config.py`,
`OV_DYN_INIT=1`), not on-device. Practical implication: **live sessions must start with the headset
briefly still, facing something textured, before moving** — this component does not yet auto-select
init mode the way the offline pipeline can.

## Build & run

```
make -C components/tracking       # -> vio_live, pose_inject, replay_feed, ov_bench (all aarch64)
adb push components/tracking/vio_live /data/local/tmp/
# config.yaml + imu_rect.txt come from tools/vio/make_openvins_config.py against the factory
# calibration -- see that script's docstring, not duplicated here.
```

## Known limits

- Motion-to-photon latency (attributing an injected pose to a displayed frame, not just to
  `dumpsys`) is unmeasured. `research-notes/47` established display vsync is on the IMU clock, which
  is what timing needs, but the attribution itself is still open.
- Static-only on-device init (above) means a room-scale capture that starts already in motion, or
  facing a blank/close surface, will fail to track — this was the exact root cause resolved offline
  in `research-notes/53` and is not yet ported into `vio_live` itself.
