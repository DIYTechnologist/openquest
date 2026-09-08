# Controller tracker: outlier mechanism found — a sustained ~7s failure window, not scattered noise — 2026-09-08

Continuation of `research-notes/60` on the same capture, no new device time. Set out to fix the
~20% outlier rate; found what's actually causing it instead, which turned out not to be fixable
with the approach tried.

## Where the outliers actually are

Traced the specific outlier frames (from the robust-fit residuals) back through `pnp_track.py`'s
own log: **33 of 48 outliers (69%) are `method=brute`**, against brute-force being only ~28% of all
solved frames — a brute-force frame is roughly **6x more likely** to be an outlier than a
prior-guided one (31% vs 5.4%). All had *low* reprojection error (0.001-0.009, comfortably under
threshold) — the PnP solve was self-consistent, just geometrically wrong. Reprojection error alone
cannot distinguish that from a correct solve; nothing in the search checks a candidate against
anything outside the current frame.

## Fix tried: sanity-check brute-force reacquisition against the last known pose

Added `motion_prior` to `track_frame()`: when a recent (`<=0.5s`) previous pose exists, prefer the
lowest-error candidate that's *also* within a plausible displacement (`max_speed x dt`) of it,
falling back to lowest-error-overall only when nothing qualifies — the same discipline
`track_frame_prior` already has, applied to the brute-force path too.

**It didn't work.** Outlier count identical before and after (33 brute-force outliers, both runs).
Checked why directly: 71/109 brute-force frames *did* have a fresh prior available (median gap
40 ms, well inside the 0.5 s window) — so the check was engaging, just not helping.

## The real shape of the problem: a sustained failure window, not isolated bad frames

Printed frame-to-frame position jumps for every brute-force frame against its immediately
preceding solved frame. They are not scattered: **from t=10.5s to t=17.6s of the capture (about
7 seconds, over a third of it), nearly every frame is `method=brute`, jumps 150-1200 mm from the
previous frame, and the previous frame is *also* usually `method=brute`.** The tracker isn't making
occasional isolated mistakes here; for a sustained stretch it never lands on a correct
correspondence at all, and each wrong frame looks locally consistent with the *previous wrong
frame* — which is exactly why the motion-prior sanity check couldn't catch it: consistency with
recent history is not consistency with truth when recent history is itself wrong.

Checked whether this window has an obvious proximate cause (fewer blobs, dimmer/occluded LEDs,
consistent with research-notes/58/59's earlier failure modes): **no.** Blob counts through the bad
window (5-10, sampled every 4th frame) are unremarkable compared to the rest of the capture, and
intensities are mostly saturated (255) — same as everywhere else. Whatever is happening is not
"the camera couldn't see the controller well."

## Leading hypothesis, not confirmed: geometric ambiguity from having no per-LED identity

The tracker has no way to tell *which* physical LED a detected blob is — only its position. With a
handful of roughly-coplanar points (the fitted model is a real but not maximally-distinctive
constellation), some controller orientations plausibly produce a near-symmetric 2D projection where
more than one 3D-to-2D assignment fits almost equally well. Real constellation-tracking systems
avoid exactly this by giving each LED a unique identity (typically a blink/timing code decoded
across frames) rather than relying on geometry alone — something this project has not attempted
and would be a materially bigger undertaking than anything tried so far in this tracker's bring-up.
**This is a hypothesis, not a proven mechanism** — no direct test was run to confirm the ambiguity
exists at the specific orientations in this window; recorded as the leading explanation because it
fits the evidence (normal blob count/brightness, sustained rather than momentary, self-consistent
wrong solutions) better than anything else considered, not because it was verified.

## What this changes about the outlier number

`research-notes/60`'s "~20% of frames are outliers" undersells how concentrated the problem is: it
is not noise spread evenly across the capture, it is (at least) one large contiguous stretch where
the tracker doesn't work at all, plus a smaller number of scattered single-frame misses elsewhere.
A fix that helps the scattered case (better thresholds, motion priors) is not the same problem as
whatever causes the sustained one, and this session's fix only ever addressed the former.

## Open

- The per-LED-identity-ambiguity hypothesis is untested. Confirming it would mean checking whether
  multiple geometrically-distinct pose hypotheses genuinely achieve comparably low reprojection
  error during the bad window specifically (not just accepting the first one found).
- No fix attempted for the sustained-failure case itself — it needs either LED identity decoding
  (a real, separate project) or a fundamentally different temporal strategy (e.g. an actual filter
  carrying orientation uncertainty forward, not just a point estimate + gate) rather than another
  threshold adjustment.
- Whether this specific ~7s window is characteristic of general use or an artifact of this one
  capture's particular motion is unknown — only one capture has ever been analyzed this deeply.

## Housekeeping

`tools/controller_tracking/pnp_track.py` gained `motion_prior`/`--max-speed` on `track_frame()`
(kept -- it is a real, correctly-functioning safeguard for the cases it can catch, even though it
didn't move this session's headline number). No new device capture; same
`exports/controller-constellation-2026-09-07/cap9b/` dataset as the last four notes.
