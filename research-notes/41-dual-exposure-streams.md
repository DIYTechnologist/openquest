# The camera stream is two interleaved exposures, not one 30 Hz feed — 2026-09-05

Chasing the "~50 Hz, not 30 Hz" anomaly from `notes/40` turned up something that matters for both
step 2 and step 3, and that contradicts an assumption the project has carried throughout.

## The measurement

Capture timestamps for a single camera are **bimodal**, not uniform:

```
inter-frame deltas (ms): 17.85, 22.14, 17.85, 22.19, 17.80, 22.18, 17.82
                         17.8 + 22.1 = 40.0 ms exactly
```

40 ms is `notes/10`'s "rock-solid 40.00 ms cadence (25 Hz)" per stream. So this is **two interleaved
25 Hz streams offset by ~17.8 ms**, which reads as ~46-50 Hz if the alternation is ignored.

The two streams are not duplicates of each other, and not the same thing:

```
consecutive frames   mean |diff| = 32.5      <- large
same-parity frames   mean |diff| =  2.0-3.5  <- small (static scene, as expected)

even parity: mean 73.2, p99 218
odd  parity: mean 40.8, p99 118
```

On a stationary headset, frames from one camera under one setting should barely differ — and
same-parity frames do barely differ. Alternate frames differ by 32.5. They are **two different
exposures of the same camera**, interleaved.

Confirmed directly from the descriptor, which carries exposure and gain per block (f64 at +7, +8):

```
exposure 0.005263 s  gain 5.0     531 frames
exposure 0.003838 s  gain 3.0     292 frames
exposure 0.005016 s  gain 4.0     241 frames
```

Auto-exposure drifts the exact values, but exposure x gain separates into a bright class (~0.020-0.026)
and a dim class (~0.0115), matching the measured pixel means of 73 and 41.

## Why this matters

**For step 2:** a VIO dataset must use **one** exposure class. Naively taking all frames at "50 Hz"
would feed OpenVINS an image sequence whose brightness flips every frame — photometric front ends
handle that badly, and it would have been very hard to diagnose from a poor ATE number afterwards.
The real per-camera rate for VIO is **25 Hz**, not the 30 Hz assumed since `notes/18`.

**For step 3:** this is the short-exposure stream `notes/18` predicted. Step 3's open question — "if
camera-based: how many IR blobs per frame in the short-exposure frames we currently discard" —
now has an instrument. The dim class is almost certainly the controller IR-LED exposure, and it is
already being captured and labelled. Step 3 got cheaper without being worked on.

`tools/cam_tap/ibfs_hook9.c` now records exposure and gain per frame in the index, so classification
is done offline from data rather than by a hardcoded rule that auto-exposure drift would break.

## Also fixed: capture is now dense

`notes/40` recorded that static-scene capture was sparse because the pixel hash does not change when
nothing moves. The trigger is now the descriptor, not pixel content — justified by measuring that
the entry's `id` equals the camera's own block index in **1185 of 1188** frames (the 3 misses are
warmup). Result:

```
before (hash-driven):  frequent gaps, 452 frames over 9 s of a 25 s window
after (descriptor):    gaps >50 ms = 0, spacing median 21.8 ms, p95 22.2, max 22.6
                       451 unique per camera, stereo pairing 451/451 (100 %)
```

Perfectly regular, and independent of scene content. Row-level duplicates remain (an emit-path race
across delivery threads) and are still deduped offline by `(cam, capture_ts)` rather than by adding
a lock inside Meta's tracking thread.

## Open

- Capture still spans only ~9 s of a longer GO window; the frames within it are gapless, so
  something ends the session rather than dropping frames. Not yet diagnosed.
- The 30 Hz figure elsewhere in the project (`notes/22`'s B2 acceptance, `sb_survey`'s 29.6 Hz
  type-0x51 exposure packets) comes from the direct-camera path, which may genuinely be configured
  differently from what `trackingservice` requests. Worth reconciling before any rate claim.
