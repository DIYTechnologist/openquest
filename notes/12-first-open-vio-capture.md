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

---

## Session 2 (2026-09-02): deeper diagnosis. Two real discoveries; Basalt still not converging.

### NEW TOOL: `tools/vio/epipolar_check.py`
Matches SIFT features between two simultaneous frames, unprojects through KB4, and scores every
ordered factory camera pair by epipolar error. Answers "are these images geometrically consistent
with the calibration, and which physical pair are they?" without needing the device.

**Result on our capture: pair (0,2), median epipolar error 0.173°, 362/407 inliers** — an order of
magnitude better than any other pairing. So our v4l2 indices *are* factory cameras 0 and 2, the
factory extrinsics are accurate for our images, and the pair is richly matchable.

Run on the known-good `exports/vio-precise` control for comparison: its images are physically
**(2,0)** — the reverse order — at a much poorer 2.04° / 134 of 635 inliers. Worth knowing: the
control converges despite a far worse geometric fit than ours.

### KEY DISCOVERY: an ~816 ms camera↔IMU time offset
Cross-correlating optical-flow magnitude (KLT, converted to rad/s via the 190 px/rad focal)
against gyro magnitude:

| lag | correlation |
|---|---|
| 0 ms | 0.380 |
| **816 ms** | **0.946** |

So **the 0xe0 exposure timestamps and the 0x50 IMU timestamps do NOT share an epoch**, despite
both being u32-microsecond fields in the same syncboss stream. The offset is ~24.5 frame periods,
which is why the earlier ±2-frame and ±3-frame sweeps could never have found it. `CAM_SHIFT_MS`
(default 816) now corrects it in the builder; after correction the residual lag is −20 ms at
correlation 0.946.

**Tool change for the next capture:** `cam_direct` now writes `syncboss_chunks.csv` —
a CLOCK_MONOTONIC stamp against the byte offset of every read of the syncboss stream. That lets
the nRF↔monotonic map be fitted *directly* rather than inferred by pairing two uniform 30 Hz
sequences (which is degenerate under integer frame shifts). This removes the guesswork at source.

### WHY the offset exists (resolved 2026-09-02)
The `0xe0` exposure timestamps are **relative to camera-stream start**; the `0x50` IMU timestamps
are on the **absolute MCU clock**. Evidence: the first `0xe0` timestamp is 33521 us = **1.006
frame periods**, i.e. the strobe clock starts from zero the moment streaming begins. The offset is
therefore the absolute MCU time at which `syncboss_camera_start_streaming()` ran — 816 ms here, of
which ~329 ms is the three request/response round trips inside `syncboss_lib_start_streaming()`
(each waits for an MCU reply).

**Consequence: 816 ms is session-specific and must be measured per capture.** `CAM_SHIFT_MS`
carries a warning to that effect. The `syncboss_chunks.csv` host timestamps that `cam_direct` now
records are the clean way to pin it, since they relate the MCU clock to CLOCK_MONOTONIC directly
rather than by pairing two uniform 30 Hz sequences.

### Also verified clean this session
- **My PNG writer**: re-encoded the control's images with it — control still converges
  (42 poses, 0.3776 m). Encoder exonerated.
- **Feature quality**: our frames are *better* than the control — mean |grad| 6.73 vs 4.36,
  corner strength 26182 vs 7361, and KLT track survival 1.00 vs 0.91.
- **Optical flow agrees with the gyro** across the capture once the 816 ms offset is applied.

### Additionally eliminated
all 12 ordered camera pairs **with IMU enabled** (the earlier sweep used `--use-imu 0` and was
therefore meaningless); IMU sign/order conventions (negated gyro, negated accel, swapped groups);
leading all-zero-gyro rows; uniform-width timestamps (raw nRF stamps crossed a digit-count
boundary, which would break any lexicographic consumer — fixed via `TIME_BASE_NS` regardless);
dataset size (45-frame subset matching the control); all three alternative Basalt configs;
and the control's (2,0) camera ordering convention.

### Where this leaves it
Every input has now been independently validated — images, geometry, calibration, IMU values,
IMU/camera time alignment, PNG encoding, timestamps, and the Basalt invocation itself (which
reproduces the control exactly). Basalt nevertheless NaNs on the first optimisation step.

**Next, in order:**
1. **Get observability.** The container only builds `basalt_vio`; add `basalt_opt_flow` (or a
   debug build with keypoint/observation counts) so we can see whether the front end produces
   observations at all. Everything so far has been inferred from a single NaN message. The GUI
   path is blocked headless by GNOME/XWayland auth — a screenshot-capable X session or an extra
   build target is the way through.
2. **Cross-check with a second VIO** (OpenVINS or Kimera) on the same EuRoC dataset. If another
   estimator converges, the fault is Basalt-specific configuration rather than our data.
3. **New capture using `syncboss_chunks.csv`** to build the clock map directly, and framed on a
   textured wall ~2 m away with slow translation and no hands/body in frame.
