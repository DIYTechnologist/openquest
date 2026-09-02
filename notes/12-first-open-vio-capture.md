# First open-stack VIO capture — dataset built, Basalt not yet converging (2026-09-01)

Follows [[11-camera-kernel-path-probe]] (which records B1: cameras + IMU driven from our own
process). This note covers turning that into a Basalt dataset. **Status: the capture and the
dataset are good; Basalt diverges on it while the older leech-based dataset still converges.**
The remaining cause is narrowed but not identified — see "What is still unexplained".

Data: `exports/vio-direct-2026-09-01/` — `raw/` (895 MB, 3010 frames), `syncboss.raw` (1.07 MB),
`frames.csv`, and the built `euroc/` dataset (318 MB).
Tools: `tools/cam_direct/run_capture.sh`, `tools/vio/build_euroc_direct.py`,
`tools/vio/make_pair_calib.py`.

## The capture (headset worn + moving, 25 s)
`cam_direct capture 0 25 8000 255` — cameras 0 and 2 plus the syncboss IMU, one process, with
`trackingservice`, the Android framework and the sensors HAL all stopped.

| | |
|---|---|
| frames | 1505/camera over 25.2 s (59.6 Hz raw = alternating SLAM + controller exposures) |
| SLAM frames after filtering | 753 / 752 (~30 Hz) |
| IMU | 25152 samples @ **993.8 Hz**, type 0x50 |
| exposure packets | 773 × type 0xe0 @ 29.6 Hz, counter contiguous 1..773 |
| image quality | sharp, well exposed, strong texture (see `raw/c0_000631.gray`) |

## Decisions made building the dataset, and why

1. **Keep only the bright half of the frames.** The Quest interleaves long-exposure SLAM frames
   with very-short-exposure controller-IR frames at ~60 Hz total. Only the former carry scene
   texture. Filter on mean intensity (threshold 20; the two populations sit at ~85 and ~4.5, so
   the split is unambiguous).
2. **Drop row 0, not row 480.** The 481st row is metadata, but it is the **first** row, not the
   last: row 0 reads mean ~6 with a distinctive `00 00 01 ff ff ff 01 a5` marker. The first
   version of the builder dropped the last row, which left the garbage row in as row 0 and
   shifted every image one row off the calibration. Fixed; `META_ROWS = 1`.
3. **Express the whole dataset in the nRF timebase, not CLOCK_MONOTONIC.** Frame stamps taken
   from the V4L2 buffer are *completion* times and jitter by ~ms against the true exposure
   instant — and [[08-vio-status]] records that exact jitter as the sole previous convergence
   blocker. The MCU gives the real exposure time in the 0xe0 packets, so every frame is snapped
   to its 0xe0 strobe and the IMU is used at its native nRF timestamps (`ts_ns = nrf_us*1000`).
   This is the same convention as the known-good `exports/vio-precise` dataset and removes the
   mono↔nRF mapping entirely. Result: frame dt becomes exactly 33.33 ms and 748/752 stereo pairs
   share byte-identical timestamps.
   *Why this matters:* a least-squares mono↔nRF fit is **degenerate under integer frame shifts** —
   two near-uniform 30 Hz sequences fit equally well at any n×33 ms offset — so the fitted offset
   could not be trusted. Snapping sidesteps the ambiguity instead of guessing at it.
4. **Pair the two cameras explicitly.** EuRoC/Basalt matches stereo frames by *exact* timestamp.
   Before snapping, only 5 of 753 matched and Basalt emitted exactly 5 poses. The exposures are
   genuinely simultaneous (FSIN fires both; median offset 118 µs) — the residual is dequeue-side
   jitter — so pairing by nearest neighbour and stamping both with one time is correct, not a fudge.
5. **Require IMU lead before the first frame.** The strobe starts before the IMU stream comes up,
   so the first ~0.75 s of frames have no IMU to integrate. Drop them (`IMU_LEAD_NS`).

## Verified good (so these are NOT the problem)
- **IMU decode.** No NaN/inf, no outliers, max |ω| 2.49 rad/s, max |a| 13.45 m/s², dt a steady
  1.006 ms with zero non-positive steps. Gyro integration predicts the accelerometer's gravity
  direction with 15.4° median error over 25 s — *better* than the control's 19.4°.
- **Basalt invocation.** The identical command on `exports/vio-precise` reproduces the known
  result: **42 poses, 0.378 m path**.
- **Images.** cam0 and cam1 at the same timestamp show clearly overlapping scene content.

## Eliminated by experiment
| hypothesis | test | result |
|---|---|---|
| camera–IMU time offset | swept IMU shift ±3 frames (±100 ms) | all NaN |
| frame↔strobe pairing | swept `KOFF` ±2 on snapped stamps | all NaN |
| wrong camera pair / order | all 6 ad-hoc calibs, then **all 12 ordered pairs** generated from the factory calibration | all fail identically |
| image orientation | vertical, horizontal, both flips | all NaN |
| insufficient IMU lead | 0.5 s, 3 s, 6 s | all NaN |
| aggressive motion | isolated calm (mean \|ω\| 0.098 rad/s) and mid windows | all NaN |
| metadata row | both conventions | all NaN |

## What is still unexplained
Basalt NaNs on our dataset (`SO3::exp failed! omega: nan nan nan`) essentially at filter setup,
while converging on the leech dataset with the same calibration, config and binary.

**A false lead, recorded so it is not repeated:** `--use-imu 0` on our data produced 719 poses,
which looked like "the images are fine, so it must be the IMU". It is not a valid discriminator —
the *control* is equally degenerate with `--use-imu 0` (45 poses, path 0.0000 m). Stereo-only
never tracks in this build, so that flag proves nothing.

### Next steps, highest value first
1. **Build Basalt with debug output / run with `--show-gui 1`** to see the optical-flow and
   keypoint state on the first frames. Everything so far has been black-box; this is the cheapest
   way to see whether features are found at all.
2. **Check the physical identity of the v4l2 camera indices.** All 12 ordered pairs failing the
   same way is suspicious — it suggests the failure is upstream of the extrinsics. But it remains
   possible that our v4l2 index → physical camera mapping differs from the factory calibration's,
   in which case *every* pairing is wrong in the same way. Resolve by capturing all 4 cameras
   simultaneously in a scene with an identifiable landmark and matching each against the factory
   `mount-angle`/extrinsics.
3. **Cross-check with a leech capture taken in the same session** so the two pipelines can be
   compared frame-for-frame on identical scene content.
4. Consider a shorter, gentler capture (slow translation, distant textured wall, no hands/body in
   frame) — our capture is dominated by the wearer's own arms and torso at very close range, which
   is poor VIO input regardless of the above.

## Reversibility / safety
All of this is host-side analysis of already-captured data. The only device interaction was the
capture itself, which restored `trackingservice`, the framework and SELinux Enforcing on exit
(verified). Nothing was flashed.
