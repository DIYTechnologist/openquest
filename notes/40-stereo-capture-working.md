# Full-rate stereo capture works; `k` == calibration index proven — 2026-09-05

## `k` is the factory calibration index — measured on all six pairs

`notes/39` matched the camera *pairs* but left two binary ambiguities. `tools/vio/epipolar_check.py`
settles them: it matches features between two frames, unprojects through the KB4 model, and scores
every **ordered** factory pair by epipolar error.

```
image pair    best ordered pair   median epi err   inliers      runner-up
k0 vs k1           (0,1)             0.131 deg      29/39        7.02 deg
k0 vs k2           (0,2)             1.840 deg      11/29       10.86 deg  ((2,0))
k0 vs k3           (0,3)             0.166 deg      20/29        6.39 deg
k1 vs k2           (1,2)             3.527 deg      10/31
k1 vs k3           (1,3)             0.326 deg      32/43
k2 vs k3           (2,3)             0.188 deg      19/30
```

**All six select the identity ordering**, with 6x-50x margins over the runner-up. So `k == n` in
`camera_calibration_v2.json`, and the intrinsics/extrinsics can be attached directly. This was
measured rather than assumed, deliberately — four consecutive inferences about this mapping had
already proved wrong (`notes/33`-`notes/38`).

The two higher residuals (`(0,2)` 1.84, `(1,2)` 3.53) both involve `k2`, whose frame in that
particular 4-way dump was written at a slightly different moment — the dump freezes each buffer's
*last* write, which is not perfectly simultaneous across cameras.

## Full-rate stereo capture: `tools/cam_tap/ibfs_hook9.c`

Dumps a chosen camera pair from our own dmabuf mappings, stamping each frame with the capture time
from that camera's own descriptor block. Defaults to cameras 0 and 2 — the only pair with real
overlap (19.6 deg, 11.2 cm baseline); every other pair is 67-82 deg apart and useless for stereo
matching (`notes/12`).

Measured over a 25 s desk run:

```
cam0: 452 unique capture timestamps
cam2: 452 unique capture timestamps        <- exactly equal
cam0 stamps with a cam2 stamp within 5 ms: 452/452 (100 %)
median spacing 21.9 ms
```

Equal counts and 100 % pairing is the acceptance condition for a stereo dataset, and it is met.

### Two defects found and handled

1. **The pixel hash fires more than once per frame** — a buffer can be sampled mid-write. First
   measurement was cam0=543 vs cam2=711 over the same window, where a stereo pair must be equal.
   Fixed by deduping on the descriptor's capture timestamp, which changes exactly once per frame.
2. **A single last-value check was not enough**: FrameSets arrive on **two interleaved streams**
   (`notes/10`'s DualStream), so a timestamp reappears after one from the other stream. A 16-deep
   ring per camera brought the unique counts into exact agreement.

Row-level duplicates still remain (523 rows for 452 unique on cam0) because the emit path races
across delivery threads. **Deliberately not fixed in the hook**: the data is correct, the index
carries the capture timestamp, so deduping offline by `(cam, capture_ts)` is free — whereas adding
a lock inside Meta's tracking thread risks the very stall the whole design has been avoiding.

## Two open questions, recorded not resolved

- **Rate is ~50 Hz, not 30 Hz** (21.9 ms median spacing, consistently on both cameras). The project
  has assumed 30 Hz throughout. Either the cameras genuinely run faster than believed, or the
  descriptor's `+11` field is not the plain capture time. It does not block a dataset — the stamps
  are consistent and pair perfectly — but it must be settled before any timing claim.
- **Capture is sparse on a static scene**: 452 frames spanning only 9 s of a 25 s window, with gaps
  up to 240 ms, because a motionless scene often leaves the sampled bytes unchanged so the hash does
  not fire. Under motion this should be dense; that is untested.

## Status

The capture chain is complete end to end: open pixels at frame rate, correct camera identity,
correct calibration assignment, true per-frame exposure timestamps, and Meta's poses plus IMU on the
same clock. What is missing for an ATE number is a **capture with motion** — the existing motion
dataset (`notes/37`) predates the camera fix and holds only one camera.

Device restored: `prox_open`, `trackingservice` clean, Enforcing. UFS soak **7 h 06 m, 0 errors**.
