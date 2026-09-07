# The VIO blocker: raw vs rectified IMU frame — 2026-09-03

Resolves the open problem in `notes/13-CHECKPOINT-vio.md`: no VIO had ever produced a valid
trajectory from real data. OpenVINS drifted ~500 m over 23 s on the reference capture; Basalt
showed zero stereo observations.

**Root cause: the EuRoC builder fed RAW IMU samples while every camera→IMU extrinsic we export is
expressed in the *rectified* IMU body frame.** The factory calibration carries a per-sensor
`RectificationMatrix` that is very nearly a 180° rotation, so the IMU and the cameras were ~180°
apart. One-line class of bug; it cost the whole investigation.

Result on the reference capture `exports/vio-table2-2026-09-02`:

| | final ‖p‖ | path length | max speed | features passing chi² |
|---|---|---|---|---|
| raw IMU (what we had) | 1246 m | 1330 m | 734 m/s | 0 |
| rectified IMU | **0.56 m** | 12.0 m | 1.80 m/s | 3428 |

Reproduced on an independent capture `exports/vio-direct-2026-09-02`: 269.55 m → **0.41 m**
(chi² passes 165 → 2021). Plot: `exports/vio-table2-2026-09-02/trajectory_rect.png`.

## The mechanism

`exports/calibration-2026-08-30/openvr_calib_out/intermediate.json` holds, per sensor:

```
gyro  RectificationMatrix ≈ [[ 0 -1  0], [-1  0  0], [ 0  0 -1]]   det = +1.0027
accel RectificationMatrix ≈ [[ 0 -1  0], [-1  0  0], [ 0  0 -1]]   det = +1.0018
```

i.e. `body ≈ (−y, −x, −z)` of raw, plus small scale/cross-axis terms. The syncboss FIFO delivers
raw sensor axes. Applying the matrix is mandatory before the data can be combined with any
extrinsic.

Why nothing static caught it: **the gyro and accelerometer rectifications are nearly identical**,
so the two stay mutually consistent in the raw frame. The accelerometer still reads a clean 1 g at
rest, and gyro-vs-accel attitude closure still passes (correlation 0.976, ~8° over 23 s, not
growing). Only the camera↔IMU relation is broken. The filter then finds every visual update
inconsistent with propagation, triangulation fails, chi² rejects the survivors, and it silently
dead-reckons — which looks exactly like "VIO that drifts", not like "VIO with a frame bug".

### Offset convention

`corrected = RectificationMatrix @ (raw − Offset)`. Established from the stationary segment — it
is the only convention that reduces the residual rate:

| | still-period ‖ω‖ |
|---|---|
| raw | 0.0562 rad/s |
| `Rect@raw` | 0.0564 |
| `Rect@raw − off` | 0.0961 |
| **`Rect@(raw − off)`** | **0.0116** |

Gyro `ConstantOffset` = [0.0620, 0.0040, 0.0101] rad/s, against a measured raw still-period bias of
[0.0540, 0.0124, 0.0095] — independent confirmation that the factory numbers describe *this* data.

## How it was confirmed, before changing anything

Rotate the gyro into the camera frame and compare against the camera rotation measured from the
images (KLT → KB4 unprojection → robust Kabsch on bearing vectors), over 163 frame pairs during
motion. Median angle between the two rotation axes:

| gyro frame / extrinsic | median | frac < 20° |
|---|---|---|
| raw / `R_ItoC` | 101.4° | 0.02 |
| raw / `R_CtoI` | 97.7° | 0.01 |
| **rect / `R_ItoC`** | **9.0°** | **0.88** |
| rect / `R_CtoI` | 26.2° | 0.21 |

This also settles the extrinsic convention question left open in `notes/08`: `R_ItoC = T_CtoI ᵀ`,
as `make_openvins_config.py` already had it. The residual 9° is the flow-based rotation estimate's
own error (translation contamination, distant-scene assumption), not a calibration error.

## Why the synthetic fixture could never have found this

`make_synthetic_euroc.py` generates IMU data *in the correct frame by construction*. The fixture
has no raw-vs-rectified distinction, so no amount of making it more realistic — scene depth,
motion blur, rolling shutter, the next steps listed in notes/13 — could ever have reproduced the
failure. **Add a third trap to the notes/13 list: a fixture cannot find a bug in a stage it does
not model.** The bisection was measuring the wrong axis entirely.

## Two real but secondary bugs, fixed on the way

Both were genuine defects, correctly diagnosed, and both turned out to change nothing once the
frame fix was in (ablation: 0.56 m / 0.55 m / 0.53 m). Kept because they are correct and will
matter for tuning. Do not credit either with the fix.

1. **`init_imu_thresh: 1.5` is ~50× this IMU's stationary accel variance (~0.03).** OpenVINS
   `StaticInitializer` takes gravity *and gyro bias* from `window_2to1` = [T−1.0, T−0.5] and guards
   it with `a_var_2to1 > init_imu_thresh`. At 1.5 that guard never fires, so a window straddling
   the pickup is accepted: it took a gyro bias of (0.0392, 0.0011, 0.0148) against a true
   still-period (0.0540, 0.0124, 0.0095) — 1.75 °/s of error, ~26° of attitude drift over 23 s, and
   it asserted `velocity = 0` 0.76 s into a brisk pickup. At 0.3 the error is 0.018 °/s. Default is
   now 0.3 in `make_openvins_config.py`.
2. **IMU noise densities were understated 21× (gyro) and 4.6× (accel).** Measured by Allan
   deviation on the 16 s stationary segment: gyro 1.85e-4 rad/s/√Hz (was 8.92e-6), accel
   2.31e-3 m/s²/√Hz (was 5.06e-4). The old values came from a Basalt config. Now measured.

## Tooling changes

- `tools/vio/build_euroc_direct.py` — applies the factory rectification. `IMU_RECT=0` to disable
  (only useful for reproducing the old broken behaviour), `IMU_CALIB=` to point elsewhere.
  Warns loudly if the calibration is missing rather than silently writing raw axes.
- `tools/vio/make_openvins_config.py` — measured noise densities, `init_imu_thresh` default 0.3.
- `tools/openvins-docker/euroc_runner.cpp` — **now actually applies the config's `verbosity`.**
  `VioManagerOptions::print_and_load` parses it but does not call `Printer::setPrintLevel`; the ROS
  runners do that themselves. Without it the level stays at INFO and every `PRINT_DEBUG` — which is
  where the MSCKF/SLAM feature counts live — is dropped. A run with no visual updates was
  indistinguishable from a healthy one. Also reports per-frame `msckf_feats`/`slam_feats`, and no
  longer feeds cam1 when `max_cameras: 1` (that threw `std::out_of_range`).
- `tools/openvins-docker/Dockerfile.debug` + `msckf_counters.patch.sh` — `openvins:dbg`, an
  instrumented build reporting per-update feature survival (`in` → `after_clonetime` →
  `after_triangulate` → chi²). Incremental: rebuilds only `ov_msckf_lib`, ~1 min. This is what
  localised the failure to triangulation/chi² rather than tracking.

## Diagnostic sequence worth reusing

1. Dead-reckon the IMU offline with a correct initial attitude. 25.6 m over 23.9 s here — normal.
   The filter doing *worse* than dead-reckoning proves vision is actively harmful, not merely weak.
2. Get the per-stage feature counts. "0 features" is the single most informative number, and
   OpenVINS will not tell you without the verbosity fix above.
3. Check tracking independently (own KLT on the dataset images): 50–120 of 200 features survived a
   full second, so the front-end was never the problem.
4. Only then look at frames/extrinsics — and validate them against the data, not against the
   calibration file they came from.

## Cross-camera validation (no ground truth needed)

All captures so far recorded only physical cameras 0 and 2, so a second *pair* is not available
without a new capture. What is available: run the same capture three ways — stereo, mono on
physical cam0, mono on physical cam2. The two mono runs use **completely disjoint image data**,
different intrinsics and different extrinsics.

Same world frame throughout (shared IMU init), so trajectories are directly comparable with no
alignment:

| | RMS | median | max |
|---|---|---|---|
| stereo vs mono cam0 | 0.020 m | 0.019 m | 0.064 m |
| stereo vs mono cam2 | 0.065 m | 0.038 m | 0.361 m |
| mono cam0 vs mono cam2 | 0.075 m | 0.052 m | 0.388 m |

over 23.5 s / ~12 m of path; path lengths 11.98 / 12.06 / 12.53 m, a 4.5 % spread.

**Metric scale.** Stereo takes its scale from the known 11.2 cm baseline; mono-inertial takes it
from the accelerometer. These are independent, and they agree: median per-step displacement ratio
**1.0011** (cam0) and **1.0024** (cam2), i.e. ~0.2 %. Use the per-step ratio, not a global
least-squares scale fit on positions — the latter gives 0.39 % / 5.27 % because it is dominated by
accumulated drift on a loop-shaped path, which is a drift measurement wearing a scale costume.

**What this does and does not show.** It bounds per-camera calibration and front-end error, and
cross-checks metric scale between two independent sources. It does **not** validate anything
common-mode: all three share one IMU, one initialisation, and one factory calibration file, so a
systematic IMU or global-frame error is invisible here. Still no absolute ground truth.

### IMU-free cross-check: stereo visual odometry (`tools/vio/stereo_vo_check.py`)

This *does* close the common-mode gap, and needed no new infrastructure — KLT, KB4 unprojection and
Kabsch were already written for the frame-validation above, and OpenCV supplies the rest. Estimate
the trajectory from the **images alone**, taking metric scale from the known 11.2 cm stereo
baseline instead of the accelerometer: triangulate cam0↔cam1 per frame, then `solvePnPRansac` into
the next cam0 frame, and chain. `imu0/data.csv` is never opened.

On the reference capture, 1156 poses, 38 failed steps, median 118 PnP inliers:

| | VO (no IMU) | VIO |
|---|---|---|
| path length | 11.71 m | 11.98 m |
| net displacement | 0.57 m | 0.56 m |

rigid alignment (scale forced to 1) **RMS 0.111 m, median 0.074 m** over 23.5 s; median per-step
displacement ratio VIO/VO **1.0087**, i.e. accelerometer-derived and baseline-derived scale agree
to **0.9 %**.

Two traps in reading this. First, the *full* VO run reports 26.75 m path / 4.12 m net — that
includes the 16 s stationary lead-in, where VO random-walks with nothing to constrain it; only the
overlap window is comparable, which is why the tool prints both. Second, the fitted similarity
scale is 0.8642, which looks like a 13.6 % scale error and is not one: on a loop-shaped path a
global scale fit absorbs accumulated drift. Use the per-step ratio.

Still not fully independent — VO and VIO share the camera intrinsics and the baseline, so a
calibration error stays common-mode. But the IMU is entirely outside the VO path, so
accelerometer scale, gravity and bias are now genuinely cross-checked.

### Independent estimator: Basalt — still not usable (negative result)

The natural way to close the common-mode gap is a second estimator. Rebuilt the rectified pair from
the corrected dataset (`rect_v2`, focal 190, IMU carried through unchanged and verified) and ran
Basalt. It dies in the **optical-flow front end** on the first frame:

```
Sophus ensure failed ... SO2Base<...>::normalize()
Complex number should not be close to zero!
```

**Control: the old raw-IMU `rect/` dataset crashes identically**, so this is pre-existing behaviour
of `basalt-vio:patched` on this rig, not something the IMU fix introduced. The rectified images
themselves are fine — a well-exposed, well-textured indoor scene, 0.5 % black corner from the warp
(`/tmp/rect_sample.png` in-session; regenerate with `rectify_pair.py`).

Consistent with notes/12's conclusion that Basalt's front end is the wrong tool for this rig.
Getting an independent estimator would mean debugging Basalt's optical flow (degenerate patches,
`optical_flow_max_recovered_dist2`) or bringing up a third one. Not attempted — the cost is not
justified while the cheaper and more direct route to the same confidence is a ground-truth capture.

## Status

Open camera + IMU stack: done (notes/11). **Open VIO on real data: converged on two captures, and
self-consistent across three camera configurations to 2–7.5 cm RMS with ~0.2 % scale agreement.**
Not yet done: no absolute ground truth, so there is still no true error number — only convergence,
plausibility (bounded drift, correct lift-off signature, sane speeds) and internal consistency.
