# Controller tracker: temporal prior + static-source rejection — sub-cm median, ~27% outlier rate — 2026-09-08

Continuation of `research-notes/58` on the same capture, no new device time. Two real, connected
bugs found and fixed, each caught by an unexpected downstream symptom rather than by inspection —
the pattern this whole tracker's bring-up has followed throughout.

## Bug 1: the "fast" tracker locked onto the wrong, stationary object

Added a prior-guided correspondence path to `pnp_track.py`: once a pose is known, project the model
into the image, match detected blobs to their nearest prediction (gated by distance), refine with
that pose as the initial guess, and only fall back to `research-notes/56`'s brute-force search on
loss. This is both far cheaper (no combinatorial search once tracking is established: 2.8 s for the
whole 499-frame dataset, vs the multi-minute brute-force-per-frame runtime `research-notes/58`'s
richer capture needed) and, in principle, self-disambiguating — a wrong correspondence has to also
be consistent with where the LEDs were predicted to be, not just look locally plausible in isolation.

It "worked" in the sense of running fast and producing a smooth, jump-free trajectory. It was also
completely wrong: the recovered path spanned **1 cm total** across the whole 20 s capture, against
a ground truth of several metres. Checked by printing raw detected blob pixel positions across the
capture: the five largest, brightest blobs sit at the same pixel position (sub-pixel jitter only)
from frame 0 to frame 498, while a separate, smaller/dimmer cluster moves substantially frame to
frame. **There is a completely static IR source in every frame, and it is brighter/bigger than the
one actually being tracked.** Most plausibly the *other* Touch controller, sitting on the desk in
view the whole session with some LEDs still lit at rest — both controllers were established as
independently visible/paired back in `research-notes/57`.

The `--max-blobs` cap added in `research-notes/58` selects by pixel area, which **actively prefers**
this static source over the real, moving controller. The prior-guided path then locked onto it
immediately (a stationary target is, trivially, its own best prediction every following frame) and
never escaped, since nothing in the design re-validates against ground truth. The original
brute-force run's instability (`research-notes/58`'s jumps and its unexplained scale=0.342) is now
better explained too: brute-force explores many candidate correspondences per frame and had no
systematic bias toward the static cluster, but nothing stopped it from occasionally mixing points
from *both* physical controllers into what should have been one rigid body's worth of
correspondences — geometrically incoherent, and exactly the kind of error that would distort
recovered scale without producing an outright wrong-looking single-frame result.

## Fix: reject anything that doesn't move

Added `find_static_positions`/`filter_static` to `blob_detect.py`: sample a spread of frames,
find pixel positions that recur within a few pixels across most of them, exclude those from every
subsequent frame's detections. The camera itself is fixed for the whole capture (desk-mounted,
proximity-bypassed), so motion is the only signal available that distinguishes "the controller
being tracked" from "any other real IR source in view" — a blob detector alone cannot tell two
genuine LEDs apart by appearance. Applied to **both** `bootstrap_model.py` (cam0 found 7 static
sources, cam2 found 0 — asymmetric, plausibly because the idle controller wasn't as visible from
that angle) and `pnp_track.py`.

Re-bootstrapping with this fix produced a **7-point model** (vs 5 before), tighter and more
plausible: 52.2 mm mean spread / 99.0 mm max (vs 83.2/138.4 mm on the contaminated run) — and this
number is now trustworthy in a way the previous one wasn't, since it isn't secretly averaging in
points from two different rigid bodies.

## Bug 2: the model growing from 5 to 7 points reopened the combinatorial blowup

`track_frame`'s brute-force fallback tries every `permutations(n_blobs, k)`, but also every
`combinations(n_model, k)` model-point subset when `k < n_model` — with the model now at 7 points
and `k` capped at 5, that's `C(7,5) = 21` model subsets **multiplied onto** every blob permutation,
turning a ~1 s bootstrap frame into ~24 s. Confirmed directly by timing a single frame in isolation
before and after the fix. Fixed by also capping the number of model subsets tried (8, not all 21) —
brute-force here only needs to bootstrap or reacquire, not be exhaustive over which specific model
points it uses, unlike the blob permutations, which encode the actual unknown correspondence.

## Result: fast, and now covers 2.5x more of the capture

```
solved 465/499 frames (308 prior-guided, 157 brute-force -- the true fallback rate this data
                        needs, not the near-zero rate seen when the tracker was silently locked
                        onto the wrong, stationary object)
post speed-filter (research-notes/58's jump rejection): 343 (122 rejected)
matched to ground truth within 50 ms: 245 (was 63 -- 2.5x more validation data, because more of
                                            the capture now tracks at all)
covered span: 19.92 s (was 7.80 s)
```

```
Sim3 (Umeyama)      : ATE RMSE 0.0726 m   median 0.0105 m   max 0.308 m   scale 0.208
SE3  (scale forced) : ATE RMSE 0.1959 m   median 0.0470 m   max 0.759 m
```

Scale (0.208) is *further* from 1.0 than `research-notes/58`'s 0.342, and RMSE/max both look worse
in isolation. Median is far better (4.7 cm SE3, vs 18.8 cm before) — a real, large improvement in
typical accuracy that the RMSE alone hides.

## Added: robust refit, because a global least-squares fit lets a minority of bad frames set the scale

The speed filter only catches a jump relative to the immediately-previous *accepted* sample; a
single wrong frame that happens to land near its neighbours in time (but not on the true
trajectory) survives it, and Umeyama minimizes total squared error, so a handful of such points
pull the *entire* alignment — including scale — toward accommodating them. `controller_ate.py` now
fits once, drops points beyond 3 median-absolute-deviations of the residual, and refits on the
remainder (all points re-projected through the refit fit, so the reported numbers describe the
whole dataset, not just the kept subset):

```
Sim3, robust refit  : dropped 66/245 (27%) as residual outliers
                      ATE RMSE 0.0994 m   median 0.0018 m (1.8 mm)   max 0.415 m   scale 0.343
SE3,  robust refit  : dropped 64/245 (26%) as residual outliers
                      ATE RMSE 0.2297 m   median 0.0038 m (3.8 mm)   max 0.933 m
```

RMSE goes *up* under the robust refit — expected and correct: the excluded points are now compared
against a fit that no longer compromises toward them at all, so their individual residuals grow
even though the majority's do not. **The honest characterization of this tracker's current state**:
when correspondence is right, accuracy is sub-centimetre (1.8-3.8 mm median); it is only right
roughly 73-74% of the time in this run. Both halves of that sentence are load-bearing — quoting
either number alone (the encouraging median or the discouraging outlier rate) without the other
would misrepresent it.

Note the robust Sim3 scale (0.343) lands almost exactly on `research-notes/58`'s original 0.342 —
consistent with the earlier number being dominated by the same class of correspondence error this
session narrowed down further, not a coincidence.

## Open

- **~26-27% of frames are still wrong-correspondence outliers**, not merely close-but-imprecise —
  the robust refit's dropped points have errors up to 0.93 m, not a long tail of small ones. The
  prior-guided path's gate distance and reprojection threshold are unchanged from their first,
  untested defaults; tightening them (fewer false gate-matches accepted) is the natural next lever,
  traded against re-acquiring (falling back to the slow brute-force path) more often.
- Scale is still not 1.0 even in the robust fit (0.343) — `research-notes/58`'s open question
  (bootstrap noise vs. correspondence-search noise as the dominant source) is not resolved by this
  session; if anything the persistence of ~0.34 across two different fixes suggests it's systematic
  rather than a symptom of whichever specific correspondence bug was live at the time, which is
  itself worth further investigation.
- `find_static_positions`'s threshold (recur within 3 px in >=70% of ~15 sampled frames) is
  untested against a scenario where the tracked controller itself pauses for a long stretch, which
  would look statistic by the same test and could get wrongly excluded.
- The frame-write truncation bug remains undiagnosed and unrelated to anything in this note.

## Housekeeping

Modified: `blob_detect.py` (`find_static_positions`, `filter_static`), `bootstrap_model.py` and
`pnp_track.py` (use them), `pnp_track.py` (prior-guided path, `--gate`, `--max-prior-gap-s`,
model-subset cap in the brute-force fallback), `controller_ate.py` (robust refit). No new device
capture; all analysis on `exports/controller-constellation-2026-09-07/cap9b/`, already committed.
