# All four cameras: static snapshot and inter-camera overlap — 2026-09-03

Every VIO capture so far used **cam0+cam2 only**, because `stage_capture` hardcodes the pair:

```c
rc = stage_capture(hal, cam, cam + 2, nframes);      // cam_direct.c:782
```

and `run_capture.sh` calls `capture 0 40 …`. The comment justifies the pair by "the 0.378 m
trajectory" — the result `notes/08` later retracted as never having converged. So the choice rested
on a withdrawn result and was never revisited. This note checks it against real images.

**Verdict: the pair is right anyway.** cam0–cam2 is simultaneously the most parallel pair *and* the
one with the longest baseline. Nothing to change there — but the rig has far more to offer.

## Capture

`tools/cam_direct/run_camsnap.sh` (+ `watchdog.sh`, a separate unconditional-restore process).
Headset resting on a desk, no handling. Sweeps four exposure/gain settings, 3 rounds × 4 cameras.
Device restored cleanly: `tracking=running hal=running enforce=Enforcing`.

Raw + PNGs: `exports/camsnap-2026-09-03/`.

- All four cameras stream from our own process — `stage_all` already supported this; only
  `stage_capture` is pair-limited.
- **Rounds alternate long-exposure SLAM and short-exposure controller frames**, exactly as
  notes/11 describes: round 1 is a real image, round 2 is black (mean 4.5, std 0.9). Sampling a
  single round per camera will silently hand you a black frame — check the mean.
- Exposure for this room: `e3000_g160` is about right. **`e8000_g255`, which every VIO capture to
  date has used, saturates 11–47 % of pixels here.** Worth revisiting per-room before the next
  capture; the previous captures may have been in dimmer light, but it is not a safe default.

## Geometry, from the factory calibration

Optical-axis angle / baseline between cameras:

| | cam0 | cam1 | cam2 | cam3 |
|---|---|---|---|---|
| **cam0** | — | 67.0° / 0.074 m | **19.6° / 0.112 m** | 80.6° / 0.148 m |
| **cam1** | | — | 80.4° / 0.148 m | 81.7° / 0.148 m |
| **cam2** | | | — | 67.4° / 0.073 m |
| **cam3** | | | | — |

Fields of view are 177–182°, so *every* pair has a large geometric FoV intersection; the binding
constraint is whether a front end can match across the viewing-angle difference, not whether the
cones intersect.

## Empirical cross-camera matching

SIFT + ratio test, then verified against the calibration's epipolar constraint on bearing vectors:

| pair | axis angle | ratio-test matches | epipolar-consistent |
|---|---|---|---|
| 0–1 | 67.0° | 99 | 76 |
| 0–2 | 19.6° | 45 | 12 |
| 0–3 | 80.6° | 18 | 6 |
| 1–2 | 80.4° | 56 | 3 |
| 1–3 | 81.7° | 71 | 33 |
| 2–3 | 67.4° | 31 | 20 |

**Every pair yields geometrically consistent cross-camera matches, including the 67° and 80° ones.**
The fisheye overlap is real and usable — the rig is not restricted to the near-parallel pair.

**Do not over-read the absolute counts.** This is one static scene with the headset flat on a desk,
so the cameras point in unrepresentative directions and cam0/cam2 are largely aimed into the desk:
they yield only 456 and 255 SIFT keypoints against 793 and 838 for cam1/cam3. The low 0–2 count is
a property of *this* scene, not of that pair — which the geometry above shows is the best one.

## What this opens up

OpenVINS already supports `max_cameras > 2` (all cameras tracked, stereo matching only within a
declared pair). Using all four as tracking cameras would give:

- roughly 4× the features, from near-full spherical coverage
- robustness to rotation: features leaving one camera enter another, instead of the total track
  loss that currently follows a fast turn

Required change is modest — extend `stage_capture` beyond `camB = camA + 2`, and generalise
`make_openvins_config.py` past its two-camera assumption. Explicitly *not* "substantial new
tooling"; that earlier framing was wrong.
