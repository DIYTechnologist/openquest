# Controller tracker: scale hypothesis checked (inconclusive), thresholds tuned — 2026-09-08

Continuation of `research-notes/59` on the same capture, no new device time.

## Scale ~0.34: tested a specific hypothesis, does not hold up cleanly

`research-notes/58`/`59` left the persistent ~0.34 Sim3 scale unexplained, past ruling out a
triangulation bug. One concrete hypothesis: if the capture's motion is mostly rotational (a wrist
swing rather than a translation), and our own model's reference point (mean of the kept LED
points, physically out on the ring) sits farther from the true rotation pivot than whatever point
Meta's own convention reports, our recovered trajectory would trace a *larger* arc for the same
real angular motion — a lever-arm effect that a Sim3 fit would absorb as "scale" without there
being any true metric error in either measurement.

Checked directly: on the full (unfiltered) trajectory, path-length ratio (est/gt) was 2.95 —
suspiciously close to 1/0.34 ≈ 2.94. But on the robust-fit inlier subset specifically, the same
ratio came out to 10.5, and the radius-from-centroid ratio was 2.0 vs 2.23 on the two subsets —
inconsistent enough across different, equally-valid ways of measuring "amplitude" that this isn't a
confirmed explanation, just a hypothesis that survived one check and failed a second. **Recorded as
checked-and-inconclusive, not confirmed** — worth being honest about a plausible-sounding idea that
didn't hold up rather than reporting the one supporting number and leaving it there.

## Threshold tuning: gate/reprojection-error defaults were untested first guesses

`research-notes/59`'s prior-guided path shipped with its first-guess defaults (`--gate 0.08`,
`--reproj-thresh 0.02`) untuned. Tightened both and re-ran on the same capture + model:

| gate / reproj-thresh | solved | brute-force | matched to GT | outlier rate (robust) | SE3 median | SE3 max |
|---|---|---|---|---|---|---|
| 0.08 / 0.02 (`research-notes/59` defaults) | 465/499 | 157 | 245 | 66/245 = 27% | 3.8 mm | 0.933 m |
| 0.03 / 0.01 | 384/499 | 107 | 226 | 45/226 = 20% | 3.7 mm | 0.681 m |
| 0.015 / 0.006 | -- | -- | -- | -- | -- | -- |

The tightest setting was killed after several minutes with negligible CPU usage and no completed
frames — tightening far enough that the prior almost never satisfies its own gate turns nearly
every frame into a brute-force reacquisition, and (per `research-notes/58`/`59`) that path is
`permutations(n, k)` with a further `combinations(model, k)` factor: pathological, not just slow.

**0.03/0.01 adopted as the new default**: a real, measured improvement (outlier rate 27% -> 20%,
worst-case error 0.93 m -> 0.68 m) at a real, smaller cost (19% fewer frames solved, 8% fewer
matched to ground truth). Median accuracy is essentially unchanged (3.7-3.8 mm) — tightening the
thresholds does not make individual correct matches any more precise, it just rejects more of the
wrong ones before they're accepted. **Not claimed as an optimum** — a real search over this
2D parameter space, or a principled way to set it from the model/scene geometry rather than by
hand, is still open.

## Open

- Same open items `research-notes/59` left: scale still not isolated (now additionally: one
  concrete hypothesis for it checked and not confirmed), frame-write truncation bug undiagnosed.
- The gate/threshold tuning here was two points and a failed third, not a real sweep — there may be
  a better setting between 0.08/0.02 and 0.03/0.01, or outside that range entirely, not found.
- Whatever is causing the remaining ~20% outlier rate at the tuned setting is still not identified
  at a mechanism level (which specific correspondence pattern causes it), only its rate measured.

## Housekeeping

`tools/controller_tracking/pnp_track.py`'s `--gate`/`--reproj-thresh` defaults changed
(0.08/0.02 -> 0.03/0.01), documented inline with the measurement that justifies it. No other code
changes. No new device capture.
