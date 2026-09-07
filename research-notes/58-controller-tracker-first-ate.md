# Controller tracker: first ATE number — 15.7 cm (Sim3) / 23.7 cm (SE3) over a 3.0 m path — 2026-09-07

Closes the loop `research-notes/56` opened and `research-notes/57` made possible: a real accuracy
number for the from-scratch controller constellation tracker, against the controller's own live
tracked pose as ground truth (not the head — `research-notes/57`'s reader).

## Capture: three attempts, two real tool bugs found in `controller_pose_log`'s auto-detection

Repeated `research-notes/56`'s `cap9` (image) capture, this time with `controller_pose_log`
running simultaneously. First two attempts produced a detected "ring" with obviously-wrong
values (one component ~1e33; another exactly `(0,1,0,0)` at every slot, `pos_y` denormal garbage).
Both were the ring-detection heuristic locking onto the wrong data, not a hardware or capture
fault:

1. **Detection ran once at startup**, before the controller had been picked up. Fixed: retry for
   up to 10 s.
2. **A length-only threshold (`>= 3`, later `>= 15`) is not enough.** A real repeating structure —
   almost certainly the *other, idle* controller's own separate ring, frozen at its last pose
   (the user's own hypothesis, and the right one) — is unit-norm and stride-spaced just like the
   live one, so a length bar alone can't tell them apart. Fixed with a variance check: reject any
   candidate run whose quaternion is (near-)constant across every slot. Real tracked motion does
   not hold still to the bit; frozen/idle data does.

Third attempt: clean detection, physically plausible and smoothly-varying output, 1072-1283 changed
samples over each ~65 s window depending on how continuously the controller moved.

## Frame yield varied a lot, unexplained, but was never as bad as `research-notes/56`'s

546 → 1810 → 2442 → 2610 total frames across four `cap9` runs this session and last, all using the
identical script and duration. Growth stopped between t=5s and t=15s each time (never mid-capture
past that point) rather than at a fixed wall-clock cutoff. The frame-truncation bug
(`research-notes/41`, `research-notes/56`) is real and still undiagnosed, but this session's yield
was consistently far above the 546-frame first attempt — 998 IR-class (38 us) frames this run vs
114 before, enough for a meaningful validation window.

## A second real bug, found by the richer data: combinatorial blowup

`pnp_track.py`'s brute-force correspondence search is `permutations(n, k)` for `n` detected blobs
against `k = min(model, n)` model points. `research-notes/56`'s capture never saw more than 8
blobs; this one saw up to 16, and `permutations(16, 5) = 524,160` per frame made a ~500-frame
dataset run for tens of minutes before it was killed. Fixed with `--max-blobs` (default 9): keep
only the largest-area detected blobs before searching, since real LED blobs bloom brighter/larger
than most noise. Not a full fix for the underlying complexity, just a practical cap; a real fix
would carry a motion prior across frames instead of searching from scratch each time (still not
done — see Open).

## Result

```
tools/controller_tracking/controller_ate.py, reusing tools/vio/ate.py's umeyama() unmodified

pnp poses (pre-filter)   : 125 / 499 solved frames
post speed-filter        : 91  (34 rejected -- implied speed > 3 m/s from the previous accepted
                                sample, the same wrong-correspondence failure mode
                                research-notes/56 already flagged, now actually filtered rather
                                than just noted)
matched to ground truth  : 63 / 91 within 50 ms (median 8.8 ms)
covered span             : 7.80 s
ground truth path        : 3.024 m, bbox 0.24 x 0.43 x 0.40 m

Sim3 (Umeyama, R+t+scale): ATE RMSE 0.1569 m   median 0.1481 m   max 0.3034 m   scale 0.342
SE3  (scale forced to 1) : ATE RMSE 0.2372 m   median 0.1884 m   max 0.4282 m
```

0.237 m over a 3.02 m path is ~8% of path length — a real number, not a diverged one (`tools/vio/
ate.py`'s own diverged check, SE3 > 10x Sim3 or scale outside [0.5, 2.0], would flag this; SE3 is
only 1.5x Sim3, so this is a case where the scale flag fires but the trajectory itself is not
collapsed to a point the way the OpenVINS wrong-shift case in `research-notes/53` was).

## The scale (0.342) is the honest finding, and it is diagnostic, not a triangulation bug

Unlike monocular VIO, PnP against a metric 3D model has **no scale ambiguity** — if the model's
absolute scale and the solved poses were both correct, Sim3 alignment should already need `s ≈ 1`.
It doesn't, and that's worth taking seriously rather than accepting as expected.

Checked whether `bootstrap_model.py`'s triangulation itself has a scale bug, with a synthetic
round-trip test: place a known 3D point in the IMU frame, project it into both cameras exactly
using the real calibration, run it through the same `triangulate()` used on real data. **Recovered
position matched the true point to 0.0000 mm, scale ratio exactly 1.0000.** The geometry is exactly
right; the calibration baseline (111.7 mm between cam0 and cam2) is also a physically sane number,
not an obviously-wrong input.

So the scale error comes from somewhere real-data-specific: most likely the same correspondence-
search instability that produces the outright wrong-correspondence jumps the speed-filter catches,
just manifesting as a *systematic* bias rather than a discrete outlier when it affects many frames
similarly (e.g. a consistently-mismatched subset of LEDs under one common viewing condition), or
from sub-pixel noise in `blob_detect.py`'s centroids being amplified by the narrow 111.7 mm stereo
baseline into real depth error at the bootstrap step. Not distinguished between these here.
**Practical consequence: report the SE3 number (0.237 m) as the meaningful one for this method** —
Sim3's free scale would otherwise quietly launder a real, diagnostic error signal into a smaller
number, the same trap `research-notes/53` named for the head-pose case.

## Open

- **Root cause of the 0.342 scale is not found**, only narrowed to "real-data correspondence noise,
  not the triangulation math." Distinguishing bootstrap-noise from tracking-correspondence-noise as
  the dominant source would need e.g. re-bootstrapping from several different frame instants and
  checking whether the model's own inter-LED distances are self-consistent.
- The frame-write truncation bug remains undiagnosed, still capping every capture's usable window
  regardless of requested duration.
- `pnp_track.py` still searches every frame from scratch with no temporal prior; both the discrete
  wrong-correspondence jumps and plausibly the scale bias would shrink with one.
- `--max-blobs`'s largest-area heuristic is untested against whether it ever discards a genuine LED
  in favour of a bright reflection; no case of that was observed here but the dataset is small.

## Housekeeping

New: `tools/controller_tracking/controller_ate.py`. Modified: `pnp_track.py` (`--max-blobs`).
Capture preserved at `exports/controller-constellation-2026-09-07/cap9b/` (frames.idx,
controller_poses.csv, meta_poses.csv committed; frames.bin/imu.bin gitignored per existing policy).
Device restored: `trackingservice`/sensors-HAL/`cameramuxmodeservice` `running`, SELinux
`Enforcing`, `prox_open` sent, all test binaries removed from `/data/local/tmp`.
