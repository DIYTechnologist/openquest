# CHECKPOINT — open VIO (resume file), 2026-09-02

Read this first on resume. Project goal: replace Meta's VR blobs on a rooted Quest 1 (`monterey`,
msm8998) with an open stack. Previous checkpoint: `notes/10-CHECKPOINT-camera-tap.md` (superseded
for the camera work). Full detail: `notes/11-camera-kernel-path-probe.md` (how the cameras are
driven) and `notes/12-first-open-vio-capture.md` (the VIO work, long, chronological).

## STANDING RULES (do not break)
- **Prompt and wait for the user to type "go"** before any capture needing them to handle the
  headset. They may not see the message in time otherwise.
- **Pull raw captures to the host before deleting them on-device.**
- **Never `pgrep -f` / `pkill -f` a service name** — the pattern matches the shell running it.
  This killed our own shell and produced a bogus diagnosis. Use `pidof <exe>`.
- Device runs are fine; **do not flash partitions**. RAM/runtime changes only.
- Always run device scripts detached (`setsid`) with the watchdog, which restores unconditionally.

## STATE: what works
**The open camera + IMU stack is DONE and reproducible** (notes/11). `tools/cam_direct/` drives all
4 tracking cameras and the IMU from our own process, with `trackingservice`, the Android framework
and the sensors HAL all stopped:
- Cameras: dlopen `libqcameraoculushal.so` → `libqcameradriver.so`; `cam_format` = **112**.
- MCU: `libsyncboss.so`. `syncboss_camera_set_frame_rate` takes a **period in µs** (33333).
  `syncboss_camera_start_streaming` must run **after** the v4l2 pipeline is up.
- `/dev/video0` is single-open and the sensors HAL also serves `android.hardware.sensors@2.0`,
  so the framework must be stopped too.
- Output: 640×481 mono8 ×4, FSIN-synced, ~30 Hz SLAM frames interleaved with short-exposure
  controller frames (keep the bright half). IMU ~994 Hz.

**Reference capture:** `exports/vio-table2-2026-09-02/` — headset on a table, picked up briskly,
carried. Excitation 0.030–0.088 m/s² still → 1.7–6.8 moving (~50× step). Camera↔IMU alignment
−15 ms, flow/gyro correlation 0.967. 1194 stereo pairs. **Use this one.**

**Synthetic fixture with ground truth:** `tools/vio/make_synthetic_euroc.py`. OpenVINS tracks it at
7.13 m vs 6.95 m truth, ATE RMSE 0.653 m, bounded error. Verified exact (accelerometer reads
[0,0,9.807] at rest; double integration reproduces the trajectory to 1 mm over 20 s).

## STATE: what does not work
**No VIO has produced a valid trajectory from real data.** OpenVINS initialises on the reference
capture (698 poses) but drifts ~500 m over 23 s. Basalt cannot match the camera pair at all.

**Important correction:** `notes/08`'s "0.378 m trajectory" was never a converged result — it shows
zero stereo observations and runs out of data just before diverging. Do not treat it as a baseline.

## Tools (all committed, all work)
| tool | purpose |
|---|---|
| `tools/cam_direct/` | drive cameras+IMU; `run_capture.sh` has live timed cues |
| `tools/vio/build_euroc_direct.py` | capture → EuRoC dataset; chunk-derived clock map |
| `tools/vio/make_openvins_config.py` | factory calibration → OpenVINS YAMLs (`OV_IMU_THRESH`, `OV_MAX_DISP`) |
| `tools/vio/make_synthetic_euroc.py` | ground-truth fixture (`SYN_IMU_NOISE`, `SYN_KB4`, `SYN_EXTR`, `SYN_EXTR_MODE`) |
| `tools/vio/rectify_pair.py` | warp the divergent pair into parallel virtual pinholes |
| `tools/vio/epipolar_check.py` | which factory camera pair do two images correspond to |
| `tools/vio/check_imu_cam_extrinsic.py` | validate camera↔IMU rotation against gyro |
| `tools/vio/make_pair_calib.py` | extract an ordered pair from the 4-camera calibration |
| `tools/openvins-docker/` | headless OpenVINS (`openvins:runner`) + `euroc_runner.cpp` |
| `tools/basalt-docker/` | Basalt, plus instrumented images `basalt-vio:{tools,debug,sim,state}` |

Run OpenVINS:
```
LD=/build/ov_msckf:/build/ov_core:/build/ov_init
podman run --rm -e LD_LIBRARY_PATH=$LD -v "$PWD":/data:z openvins:runner \
  euroc_runner /data/ovconfig/estimator_config.yaml /data/euroc /data/ov.txt
```
`:z` relabeling fails on `/tmp` — keep datasets under the repo.

## The bisection, and where it stopped
Fixture made progressively realistic (truth 6.95 m): IMU noise+bias 6.17 ✓, KB4 fisheye 7.14 ✓,
real camera→IMU rotation 6.15 ✓, **full real extrinsics (19.6° divergent pair) 19.10 m** — a real
~3× degradation, matching the Basalt finding, but far short of the real capture's ~500 m.
Timing swept 0–120 ms: no coherent optimum.

### Next steps
1. **Continue the bisection into the unmodelled differences**: real scene depth distribution,
   motion blur, rolling shutter, the alternating-exposure frame selection, sensor imperfections
   beyond white noise + constant bias. Whichever reproduces ~500 m is the cause. All offline.
2. If the divergent pair proves decisive, the rig may simply need a VIO built for non-overlapping
   multi-camera rigs, or per-camera mono-inertial fusion.
3. Only then consider another capture.

## Traps that have already cost time
- **The fixture has twice manufactured plausible false findings**: identical landmark patches (KLT
  matched wrong blobs), and a frontal-only scene (real camera rotation → 7 features instead of
  300, which looked exactly like an extrinsics bug). Always check feature counts and look at a
  rendered image before believing a fixture result.
- `--use-imu 0` in Basalt is **not** a valid discriminator; the known-good dataset is equally
  degenerate with it.
- OpenVINS `has_jerk` is computed from image **disparity**, not the accelerometer. Raising
  `init_max_disparity` too far makes everything read "still" and static init is never attempted.
- OpenCV's YAML parser rejects a trailing comment on a bool (`use_stereo: false # x` silently
  does nothing).
- `CAM_SHIFT_MS` in the builder is **session-specific**; captures with `syncboss_chunks.csv` use
  the chunk-derived map instead and should not rely on it.
- Basalt's `optical_flow_max_recovered_dist2` defaults to 0.04 (0.2 px), far too tight for a real
  stereo viewpoint change.

## Device / repo state
Device restored after every run: `trackingservice` + sensors HAL running, framework up, SELinux
**Enforcing**, nothing flashed. Repo clean; all work committed on `main`.
