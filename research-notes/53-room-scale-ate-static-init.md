# Room-scale ATE: 11.7 cm over a 60 m walk — the cause was static initialisation — 2026-09-06

`notes/51` closed the in-place case (7.6 cm) and left the room-scale walking capture open at
~2839-3905 m of drift, with two named suspects: **timestamp jitter** and **feature-tracking quality
under real motion**. Both are wrong. The cause is that OpenVINS's **static initialiser fired at the
instant motion began**, initialising with zero velocity and a gravity vector taken from an
accelerometer window that already contained real acceleration. Every visual update was rejected
from then on and the filter dead-reckoned.

Enabling and repairing **dynamic initialisation** takes the same capture, unchanged, from ~3905 m to:

```
ATE RMSE (Sim3)       : 0.1197 m          ground truth path : 60.59 m
ATE RMSE (SE3, s=1)   : 0.1238 m          bounding box      : 6.62 x 0.94 x 7.21 m
ATE median / max      : 0.105 m / 0.281 m covered span      : 72.4 s (1810/1810 poses, median 3.9 ms)
RPE, 1 s windows      : RMSE 0.1000 m
drift rate            : 0.0992 m/min
estimated scale       : 1.011
```

**This is step 2's room-scale deliverable.** 11.7 cm over 60.6 m of walking is 0.23 % of path
length, and unlike `notes/51`'s in-place number the **metric scale is genuinely observed** (1.011,
with SE3 ≈ Sim3) because walking supplies the accelerometer excitation that the in-place capture
never did. `notes/51`'s 23 %-low scale was a symptom of the motion regime, not of the pipeline.

## The two suspects from `notes/52`, measured and dismissed

**Timestamp jitter: dead.** `tools/vio/lag_and_tracking_profile.py` sweeps the camera↔IMU lag in
rolling windows instead of once globally. Over the walking capture's full 108 s, 24 of 24 windows
peak with r ≥ 0.77 (most > 0.9) at:

```
lag  mean +109.3 ms   std 3.5 ms   range [+100.0, +116.0] ms
```

A 16 ms spread over 108 s. A single constant per-capture shift is the correct model; the per-capture
part of `notes/51` stands, the "it might wander" part does not. (The control capture is equally
stable: +250.8 ± 6.3 ms.) The shift still matters enormously — re-running the *working* configuration
with the wrong 254 ms shift gives 2173 m. Both fixes are necessary and independent.

**Tracking quality: real but not the cause.** Median track lifetime is 3 frames vs the control's 6,
and left-right stereo match rate is actually **better** (78.7 % vs 55.4 %). Worse, not fatal.

Also ruled out along the way: frame drops (none; cam0/cam1 timestamps identical for all 2692 pairs,
IMU continuous at 1.006 ms), and the IMU itself — rotating the accelerometer by Meta's ground-truth
orientation recovers gravity as **9.787 m/s²** (control: 9.648) with a 10 s free-inertial drift of
5.8 m (control: 16.0 m). The walking capture's IMU is *better* than the one that worked.

## What was actually happening

`openvins:dbg` (the MSCKF counters from `notes/14`) shows the failure directly. The front-end is
fine and nearly identical between the two captures; the filter state is empty:

| | walking (diverged) | in-place (7.6 cm) |
|---|---|---|
| MSCKF candidates entering each update | 17.8 | 17.1 |
| surviving triangulation | 3.2 | 4.5 |
| **SLAM features after init** | **0.1 — zero in 94 % of frames** | **28.2 — never zero** |
| MSCKF features used | 1.1 (zero in 72 %) | 5.5 (zero in 10 %) |

With no SLAM landmarks there is no visual constraint on position at all, and the trajectory is pure
inertial integration: fitting a single constant global acceleration to it leaves a **1.8 % residual
over a 3926 m excursion**, i.e. it is textbook dead reckoning off a gravity error of ~8°
(1.38 m/s² ≈ 9.81·sin 8°).

**Why init happened at the worst possible moment.** Static init needs a stationary window followed
by a jerk. The capture's only stationary window is its first 34 s — and during it cam0 is looking at
a **blank surface at close range** (a flat object filling the frame; ~20-26 FAST corners against
cam1's ~70, and OpenVINS logging `not enough feats to compute disp: 0,97 < 15` for the whole
period). It is not an exposure fault; the frames are all distinct and correctly exposed, the
headset was simply set down facing something featureless. So init could only fire on the jerk, which
by definition arrives when the device starts moving — and by then `|w|` is 0.67 rad/s and accel std
is 2.0 m/s². Static init's core assumption is violated exactly when it is applied. cam0 does not
recover to hundreds of corners until t ≈ 45 s, giving the already-diverged filter ten blind seconds.

The control capture escaped this only because its still window happens to be well-textured on both
cameras.

## The fix, and the honest caveat on it

`make_openvins_config.py` **already contained the correct diagnosis in a comment** — that static init
"fires mid-motion and initialises with zero velocity and gravity aligned to an accelerometer reading
that includes real acceleration", that this "is consistent with the large drift we saw", and to
"use dynamic init". It then sets `init_dyn_use: false`. A previous session found the right cause,
tried the right fix, hit a Ceres failure, set `init_dyn_mle_max_iter: 0` on the reasoning that "the
linear solution alone is sufficient to bootstrap", and reverted. **That reasoning is wrong**: with
the MLE refinement disabled the linear recovery is singular — `covariance recovery failed` ×1306,
and on the rare occasions it solves it returns `|v| = 0.0001 m/s`. Dynamic init then never fires at
all, which is indistinguishable in the trajectory output from having it switched off.

The working set is five parameters that move together, now behind `OV_DYN_INIT=1` in
`make_openvins_config.py` (config preserved at `exports/step2-motion3-2026-09-05/ovconfig_dyn/`):

```
init_dyn_use          false -> true
init_dyn_mle_max_iter     0 -> 50      <- the one that was actually load-bearing
init_dyn_mle_max_time  0.05 -> 1.0
init_window_time        1.0 -> 3.0     <- 1 s does not observe enough parallax
init_dyn_num_pose         6 -> 8
init_dyn_min_rec_cond  1e-12 -> 1e-15
```

**It is not a universal improvement, and is therefore left OFF by default.** On the in-place capture
— which *does* have a good still window — the same config **regresses it from 7.6 cm to 1215 m**,
because dynamic init wins the race against static init and is degenerate without excitation. The
rule is: **static init when there is a stationary, well-textured window; dynamic init when there is
not.** Trimming alone does not substitute — the trimmed dataset with static init never initialises
at all (0 successful inits, empty trajectory).

Attribution runs, all on the same data:

| run | ATE (Sim3) | ATE (SE3) | scale |
|---|---|---|---|
| walking, static init, 110 ms shift (`notes/51`) | — | ~3905 m | — |
| walking, **dynamic init, 110 ms shift** | **0.1197 m** | **0.1238 m** | **1.011** |
| walking, dynamic init, trimmed to t>44 s | 0.1169 m | 0.1202 m | 1.009 |
| walking, dynamic init, wrong 254 ms shift | 2.57 m | 2173 m | 0.001 |
| in-place, static init (`notes/51`) | 0.0759 m | 0.0827 m | 0.772 |
| in-place, dynamic init | 0.128 m | 1215 m | 0.000 |

## A trap in the ATE metric itself — read the scale

Two rows above show a **Sim3 ATE of 2.57 m and 0.128 m for trajectories that diverged to 7254 m and
4686 m.** Umeyama with a free scale will shrink an arbitrarily divergent trajectory onto the ground
truth, and the "ATE" it then reports is nothing but the spread of the ground truth itself — 0.128 m
is simply the size of the in-place capture's 0.50 × 0.37 × 0.43 m box. **A Sim3 ATE is not a
divergence detector.** Always read the estimated scale and the SE3 number beside it.

`tools/vio/ate.py` is new, implements the `notes/18` step-2 protocol in one place so numbers are
comparable between captures, and now prints an explicit `*** DIVERGED ***` warning when the scale
leaves [0.5, 2.0] or SE3 exceeds 10× Sim3. It reproduces `notes/51`'s in-place figures exactly
(0.0759 / 0.0827 / 0.772 / 0.054 m·min⁻¹), which is what validates it.

## New standing rules

- **Every capture needs a stationary init window that is stationary AND well-textured on BOTH
  cameras.** Check cam0 and cam1 separately — this capture's cam0 was blind while cam1 was fine, and
  nothing in the pipeline complained. `lag_and_tracking_profile.py` reports per-window corner counts
  and blur; look at them before running an estimator.
- **`init_dyn_use` is a per-capture decision, not a default.** Wrong either way costs 4 orders of
  magnitude.
- **Never quote a Sim3 ATE without its scale and SE3 companion.**

## Scoreboard change

Step 2 (ground truth vs Meta) now has both numbers it needed: **7.6 cm in-place** (`notes/51`) and
**11.7 cm room-scale over a 60.6 m walk with correct metric scale** (here). `notes/48`'s and
`notes/52`'s caveat that no room-scale result existed is discharged. Step 2's blocking role over
steps 5 and 6B is released; step 3's controller-pose-fusion question is untouched and still open.

Everything here is offline analysis of data captured on 2026-09-05. No device time was used.
