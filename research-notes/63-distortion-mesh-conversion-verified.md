# Distortion mesh: converter fidelity verified, absolute-accuracy comparison confirmed blocked — 2026-09-09

`research-notes/42` left one thing open for 6A task 3: "reproduced to a stated pixel error," framed
as needing a comparison render against the live stock compositor. `research-notes/62` (same
session) found that comparison is a confirmed software dead end — `/dev/graphics/fb0` is never
written by the real compositor and `screencap` is refused as a protected display, the same wall as
motion-to-photon. Checked for an independent ground truth too: `research-notes/05`'s `Fisheye62`
model is the tracking *cameras*, unrelated to the headset's eye optics — no independent lens
calibration exists anywhere in this project. **So the mesh's absolute metric correctness against
real optics cannot currently be checked, on-device or off, at all.** Recorded as blocked, not
silently reinterpreted into something easier.

What *can* be checked without a device: whether `mesh_to_monado.py`'s generated C sampler actually
reproduces the source mesh it was built from, and how much bilinear interpolation between control
points could plausibly be off by. Both bound the **converter's own fidelity**, a real but
categorically weaker claim than "matches Meta's real optics" — stated as such throughout, not
conflated with the blocked comparison.

## Method

New: `tools/display/mesh_verify.c` (a tiny host-compiled harness that `#include`s the actual
generated header and calls the real `quest1_mesh_sample()`, not a Python reimplementation of it that
could silently diverge from the shipped code) and `tools/display/verify_mesh_conversion.py`, which
generates the header via `mesh_to_monado.py` itself, compiles and runs the harness, and checks two
things against the cached `exports/display-6a-2026-09-05/distortion-mesh.bin`:

**1. Exact-vertex round-trip.** At all 33x33 grid vertices per eye, the C sampler must return
exactly the source value that vertex was built from (bilinear degenerates to picking one side
exactly at t=0 or t=1). Result: **worst deviation 0.00067 px** over the 1216x1344 eye target —
float32 rounding noise, not a bug. This is the thing that could have silently broken (a
normalisation or header-emission mistake) and didn't.

**2. Bilinear-vs-bicubic sensitivity at cell midpoints.** A separable Catmull-Rom bicubic fit
through the same 33x33 control points (implemented directly, no scipy dependency in this project)
gives a smoother reference at points the mesh itself doesn't specify. Comparing it against the
shipped bilinear sampler at 1000 random cell midpoints: **median 0.31 px, 95th percentile 2.5 px,
max 7.4 px**. This is *not* a measurement of real-world accuracy — it's a bound on how much the
choice of interpolation scheme alone could move the answer between the grid's control points, which
is the only kind of "pixel error" statement available without a finer ground truth.

## What this closes, and what it doesn't

Closes: the converter itself is verified correct (round-trip) and its interpolation choice is
quantified (sensitivity bound), addressing the concrete, checkable half of "stated pixel error."

Does not close, and cannot currently be closed: whether the mesh's *absolute* mapping (the min/max
normalisation `mesh_to_monado.py`'s own docstring already flagged as "metrically provisional")
actually matches the real optics. That needs either a live comparison render (blocked,
`research-notes/62`) or an independent lens calibration source (searched for, does not exist in
this project). Left open rather than asserted either way.

## 6A / step 6 status update

| criterion | state |
|---|---|
| panel timing documented | done (`research-notes/44`) |
| distortion reproduced to a stated pixel error | **converter fidelity verified** (sub-0.001px round-trip, 0.3-7.4px interpolation-scheme sensitivity) — **absolute-accuracy comparison confirmed blocked**, same wall as motion-to-photon |
| motion-to-photon measured | blocked pending hardware (`research-notes/62`) |

Both of step 6's remaining items now terminate at the identical wall: no software path exists on
this device to observe what the compositor actually puts on the panel. Anything further on either
needs a photodiode or equivalent external capture, not more probing from software.
