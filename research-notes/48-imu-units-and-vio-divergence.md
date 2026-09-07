# A units bug in IMU rectification, and VIO still diverges — 2026-09-05

The capture this time is exactly what was asked for. The dataset checks out on every test available.
OpenVINS still diverges, and the honest position is that this reproduces a **pre-existing, documented
open problem** (`notes/13`) rather than a new one.

## The capture is right

```
stationary 0-35 s   accel std 0.0154-0.0160    (the window static init needs)
moving 35-115 s     accel std 0.8-3.7
Meta path 64.48 m   bounding box 6.62 x 0.94 x 7.21 m   <- actual room-scale walking
                    (previous attempt: 0.84 x 0.94 x 1.30 m)
2692 stereo pairs, 2692/2692 paired, 25.0 Hz over 107.7 s, 114,269 IMU samples
IMU <-> Meta rotation correlation r = 0.9987
```

## Found: the IMU offsets are in SI units, the stream is not

`build_euroc_direct.py` documents the rectification convention as `Rect @ (raw - Offset)` and reports
it taking the still-window gyro from 0.0562 to 0.0116 rad/s. Applying that literally to raw samples
reproduces almost none of the benefit. Measured on this capture's stationary window:

```
Rect @ (raw_degs - Offset)          |gyro| 0.06105 rad/s   <- offset treated as raw units
Rect @ raw only                     |gyro| 0.06214
Rect @ (raw_rads - Offset)          |gyro| 0.00772         <- correct
```

**The `ConstantOffset` is in rad/s while the stream is in deg/s** (and the accelerometer
`OffsetAtZeroDegC` is in m/s^2 while the stream is in g). Convert to SI *first*, then rectify. The
correct form gives 0.00772 rad/s at rest — better than the 0.0116 `build_euroc_direct` documents,
which suggests **the same latent units bug exists there** and is worth checking before that path is
trusted again.

`build_euroc_leech.py` now converts to SI before rectifying, and the still-window figures are
`|accel| 9.8660 m/s^2` (expect 9.807) and `|gyro| 0.00772 rad/s` (expect 0).

## What is verified, and what therefore is not the cause

| link | evidence |
|---|---|
| stereo geometry | epipolar on a real simultaneous pair: **0.272 deg median, 169/192 inliers**, next-best camera pairing 5.6 deg |
| camera identity / calibration assignment | identity ordering selected on **all six pairs** (`notes/39`) |
| IMU frame | rectified gyro matches Meta's own device-frame angular velocity on the **diagonal** (0.63 / 0.80 / 0.61, off-diagonals <= 0.25) — no permutation or sign flip |
| IMU magnitudes | 9.866 m/s^2 and 0.0077 rad/s at rest |
| frame timing | FrameSet stamps are absolute CLOCK_MONOTONIC; IMU offset from an r=0.9987 correlation; OpenVINS converges its own camera-IMU timeoffset to a plausible 10.8 ms |
| initialisation | **fixed** — 1806 poses covering 72 s, against 455 before |

So the divergence is none of those. Result: **7267 m** (9057 m before the units fix, 95-196 m on the
earlier capture that never initialised properly).

## This is `notes/13`'s open problem

`notes/13` ran a bisection on exactly this and recorded: a synthetic fixture with truth 6.95 m gave
6.17 m with IMU noise and bias, 7.14 m with KB4 fisheye, 6.15 m with the real camera->IMU rotation,
and **19.10 m with the full real extrinsics (the 19.6 deg divergent pair)** — a real ~3x degradation
from the camera geometry alone — while real captures drifted ~500 m. The bisection stopped there,
unresolved.

`notes/22` did get 0.203 m on the same 0/2 pair, but through `build_euroc_direct.py`, which differs
in ways that are now suspects rather than details: it snaps frame times to 0xe0 exposure stamps with
an 816 ms `CAM_SHIFT`, because **0xe0 stamps are relative to camera-stream start, not absolute nRF
time** — confirmed here, where they compute to 0.79 Hz if read as absolute.

The next thing to try is therefore not more capture but a **controlled comparison**: run
`build_euroc_direct`'s output and this pipeline's output through the same estimator config and
diff the datasets, since one converges and one does not on the same camera pair and the same rig.

## Not claimed

No ATE, no drift rate, no RPE. The number does not exist yet and nothing here should be read as
progress toward it beyond ruling causes out.

## Controlled diff against the converged run (added later, 2026-09-05)

`notes/22` reached 0.203 m. Its config is preserved in `exports/vio-b2-2026-09-04/ovconfig/`, so the
estimator side can be compared directly rather than guessed at.

**All three config files are now byte-identical to that run.** Two real differences were found and
removed — `downsample_cameras` was `true` in mine (halving image resolution) and
`max_slam_in_update` was 12 vs 25 — and the `kalibr_imucam_chain.yaml` and `kalibr_imu_chain.yaml`
were **already identical**, which rules out the calibration and the camera-IMU extrinsic *as
transcribed*. Result with the converged config: **7562 m**, essentially unchanged from 7267 m.

So the difference is in the data, not the configuration.

### The failure signature is dead reckoning, from the start

```
estimated speed: median 113 m/s, p90 207, max 1108
Meta's measured speed on the same capture: ~1-2 m/s
```

Integrating an unremoved 1 g for ~10 s gives ~98 m/s, which is the order of what is observed. And
trimming the dataset to 25 s of motion gives 956 m over 668 poses against 7562 m over 1810 — drift
scaling with **time**, not with motion aggressiveness. Both point at the filter propagating on IMU
alone with visual updates rejected, which is exactly what `notes/13` predicted a bad camera-IMU
rotation would produce.

### But the two captures are not comparable trajectories

Worth recording, because it changes what `notes/22`'s number means:

```
b2 (0.203 m)   |gyro| median 0.041 rad/s   54 s still + 21 s gentle, path 7.24 m, max excursion 0.62 m
leech (7562 m) |gyro| median 0.962 rad/s   35 s still + 80 s walking, path 64.5 m, box 6.6 x 7.2 m
```

23x the rotation rate and 9x the path. **`notes/22`'s 0.203 m was a desk-scale wiggle**, not
evidence the pipeline survives room-scale walking. That is not a criticism of the number, which was
honestly reported against a `< 2 m` criterion — but it should not be read as validation for this
regime.

### The camera-IMU rotation is still the prime suspect, and NOT yet measured

It is the one link neither check covers: the epipolar test is camera-to-camera only, and the
gyro-vs-Meta test is IMU-only. An attempt to measure it here — estimating camera angular velocity
from optical flow between consecutive frames and comparing against the rotated gyro — produced
143 / 138 / 88 deg median axis error across the candidate conventions, which would be damning
except that the measurement itself does not validate:

```
|w_cam| median 1.789 rad/s   |w_imu| median 1.396 rad/s
magnitude ratio 1.306        magnitude correlation r = +0.518
```

A ratio of 1.31 and r of 0.52 mean the image-derived rotation is not measuring rotation cleanly. The
cause is the capture itself: walking through a room produces large translation parallax, and a
rotation-only fit to bearing vectors absorbs that as spurious rotation. `check_imu_cam_extrinsic.py`
assumes a distant scene for exactly this reason.

**So no conclusion is drawn about the extrinsic.** The right test needs either a rotation-dominant
segment (pivot in place, little translation) or a translation-robust estimator (essential matrix,
keeping only the rotation). That is offline work on data already captured — the stationary-to-motion
transition at t=35 s may contain a usable rotation-dominant window.
