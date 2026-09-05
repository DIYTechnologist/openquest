# First genuine ATE number: 7.6 cm RMSE — and the root cause of the divergence found — 2026-09-05/06

The camera↔IMU rotation extrinsic, the prime suspect since `notes/48`, is **not** the problem — it
checks out once measured correctly. The actual cause of every VIO divergence this project has hit
is a **systematic timing error between the leech's frame timestamps and the IMU/Meta clock**, on the
order of 100-250 ms. Fixing it took the phases capture from 7562 m of drift to **7.6 cm ATE RMSE
against Meta ground truth** — the project's first genuine accuracy number. It only partially fixes
the room-scale walking capture, and that gap is not closed. Both facts are reported here.

## How the extrinsic got cleared

`notes/48`'s attempt to measure the camera-IMU rotation failed its own sanity check (magnitude ratio
1.31, r=0.52) because the capture involved walking, and a rotation-only Kabsch fit on fisheye
bearing vectors conflates translation parallax with rotation. `tools/vio/check_imu_cam_extrinsic.py`
already existed and solves this properly: it measures only the **in-plane rotation angle** via
`cv2.estimateAffinePartial2D` between consecutive **rectified** (pinhole) frames, which is immune to
translation to first order — a translating pinhole camera cannot appear to rotate about its own
centre; only true rotation does that.

Run naively on the rotation-dominant window with **no lag search**, it gave near-zero correlation
(0.096) and the tool's own verdict was "camera<->IMU rotation looks wrong". That would have been a
second false conclusion about the extrinsic, avoided by not stopping there.

## Filtering + a lag search found the real fault

Two fixes to the raw measurement, both necessary:

1. **Filter degenerate affine fits.** ~1 % of frame pairs gave `|omega|` up to 68 rad/s — physically
   impossible (458 deg/s ≈ instant blur). Rejecting `|omega|>8 rad/s` and requiring ≥20 RANSAC
   inliers removed the outliers without touching the real signal.
2. **Search the lag, don't assume zero.** `check_imu_cam_extrinsic.py` interpolates the gyro onto
   the visual timestamps directly, i.e. assumes both are on the same clock with no offset. They are
   not. Sweeping the lag:

```
rotation window (0-75s capture, 40s segment): peak r=0.893 at lag -254 ms
gentle window   (independent 37s segment)   : peak r=0.776 at lag -242 ms
```

Two independent segments of the same session agree to 12 ms, and the peaks are genuine — smooth,
symmetric, well inside the search range (unlike the false peaks-at-the-boundary seen in `notes/39`).
**The `R_i_c^T` convention (Basalt/OpenVINS's own reading of `T_imu_cam`) is confirmed correct**:
0.893 vs 0.410 for the opposite sense, over 2x separation.

## The fix, and what it is not

The frame's recorded `CLOCK_MONOTONIC` timestamp (from the FrameSet descriptor, `notes/34`) is
**~250 ms earlier than the true capture time**, relative to the clock the IMU was pinned to via
Meta-pose correlation. This is not the same latency as `build_euroc_direct.py`'s `CAM_SHIFT_MS`
(816 ms) — different pipeline, different fixed delay — but it is the same *phenomenon*: this
device's frame delivery has non-trivial latency that must be measured and compensated, never
assumed to be zero.

**It is not a single global constant.** Measured independently on the walking capture (`notes/48`),
the same diagnostic gives a *different* value:

```
walking capture: peak r=0.952 at lag -110 ms
```

The two captures' `--imu-offset-ns` (the separate Meta-pose↔gyro correlation used to put IMU on
`CLOCK_MONOTONIC`) were fit independently, each with its own small residual, so the correction
absorbs both the genuine camera latency and that per-session residual. **This must be measured per
capture**, not hard-coded, and `tools/vio/build_euroc_leech.py --cam-shift-ns` now takes it as an
explicit argument (default 254 000 000, i.e. the value from the capture that established the
method — override per session).

## Result 1: the phases capture — dramatic, and now with a real accuracy number

With the shift applied, OpenVINS's own online-optimised camera-IMU timeoffset converges to a sane
**-4.4 ms** residual (was wildly divergent before), and the trajectory holds together:

```
before shift:  ~95-196 m drift (first attempt), init failure
after shift:   1806-2104 poses tracked, actual displacement 0.16 m over 84 s
               (the "17.1 m" the runner printed is path length, not drift -- max
               per-10s excursion from segment start stayed under 0.5 m throughout)
```

This is the first capture with genuine rotation (up to several rad/s, `notes/48`) to converge at
all, let alone this well — better than `notes/22`'s previous best, which moved far less.

**With Meta ground truth captured in the same session, this is a real ATE measurement:**

```
alignment method     : Sim3 (Umeyama, rotation+translation+scale) -- standard EuRoC/TUM protocol
matched poses         : 2104 / 2104 within 20 ms (median 3.8 ms)
ATE RMSE (Sim3)       : 0.0759 m
ATE RMSE (SE3, s=1)   : 0.0827 m
ATE median / max      : 0.065 m / 0.187 m
RPE, 1 s windows      : RMSE 0.110 m, median 0.090 m
drift rate            : 0.054 m/min
estimated scale       : 0.772  (23 % low)
covered span          : 84.1 s of the ~117 s capture (188 s in, static+early-rotation excluded by
                        design -- static init needs the stationary window, then tracks from there)
```

This satisfies `notes/18` step 2's stated deliverables — ATE RMSE with a stated alignment method,
drift rate in m/min, RPE over 1 s windows — for the first time.

**Two honest limits, not glossed over:**

- **This is not room-scale.** The covered window is in-place rotation plus gentle arm's-reach
  motion (`notes/48`: 0.21×0.32×0.18 m and 0.50×0.30×0.43 m boxes), the same caveat `notes/48` raised
  about `notes/22`'s 0.203 m. A sub-10 cm number on this motion regime does not establish sub-10 cm
  accuracy for walking.
- **Scale is off by 23 %** (0.772). Accelerometer excitation over the covered window has `|a|` std
  of only 0.94 m/s², which is low for scale observability — the SE3-forced-scale-1 RMSE (8.3 cm) is
  close to the Sim3 number, so the *shape* of the trajectory is right even though the *metric scale*
  is not yet well constrained by this capture's motion.

## Result 2: the walking capture — real improvement, not resolved

Applying its own independently-measured 110 ms shift:

```
no shift        : 7562 m drift (dead reckoning, notes/48)
110 ms shift    : ~3905 m drift, OpenVINS timeoffset converges to +0.9 ms (very sane)
254 ms shift    : ~2839 m drift (worse timeoffset residual, -18.8 ms, yet lower total drift)
```

A real, large improvement (2-2.7x) but **nowhere near convergence**. The sane time-offset
convergence with 110 ms (its own correctly-measured value) but *worse* trajectory than the
mismatched 254 ms shift is a genuine puzzle — a converged internal time-offset variable is not
sufficient evidence of a correct trajectory if most visual updates are still failing. The walking
capture likely has an additional problem beyond timing: faster real-world motion invites motion
blur and harder feature tracking that the in-place rotation capture didn't stress. **Not
resolved — recorded as open, not claimed as fixed.**

## What this changes

- `notes/13`'s original open bisection problem is **substantially explained**: it is a timing bug in
  frame delivery, not the camera-IMU rotation, not the calibration, not the 19.6° divergent pair
  (`notes/13` measured that as a real but survivable ~3x degradation, not the ~1000x seen in
  practice).
- `notes/22`'s 0.203 m and this session's 7.6 cm are both genuine, both honestly caveated as
  non-room-scale, and now the project has a *second* independent convergent result rather than one.
- Room-scale accuracy remains the open deliverable for step 2. The next diagnostic step is checking
  whether the walking capture's failure is itself timing-related at finer granularity (a single
  fixed shift may be insufficient if there is jitter, not just a constant offset) versus a genuinely
  separate cause.

## Housekeeping

`tools/vio/build_euroc_leech.py` gained `--cam-shift-ns` (documented in its own docstring with the
measurement and both captures' fitted values). Datasets and OpenVINS outputs kept under
`work/vio-leech/` (gitignored, regenerable from the exports + this note's commands). No new device
capture was needed for any of this — it is entirely offline analysis of already-captured data.
