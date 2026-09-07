# First VIO run on leech frames: pipeline complete, result not yet valid — 2026-09-05

The whole step 2 chain now exists and runs end to end. The ATE number does not exist yet, for a
reason that is understood and cheap to fix.

## The capture is good — verified, not assumed

90 s handheld motion capture, cameras 0 and 2, with Meta poses and IMU alongside:

```
frames        13,765 rows -> 9,296 unique (cam,capture_ts)
per camera    cam0 4648, cam2 4648            exactly equal
parity split  2324 frames each at 25.0 Hz     (one exposure class, notes/41)
stereo pairs  2324 / 2324 within 8 ms         100 %
Meta poses    6000 samples @ 60 Hz, all 6000 distinct
IMU           99,362 @ 993.6 Hz, r=0.9988 correlation to Meta rotation
```

Two independent checks say the dataset itself is sound:

- **Epipolar geometry on a real simultaneous stereo pair: 0.272 deg median, 169 inliers of 192**,
  with the next-best camera pairing at 5.6 deg. Camera identity, calibration assignment and
  simultaneity are all confirmed on the actual data, not on a contrived dump.
- IMU and camera timestamp ranges bracket correctly after the clock offset is applied
  (25,775,902,834,716 ns, recovered by correlation at r=0.9988).

One real defect found and fixed while building: **row 0 of every frame is a metadata line, not
pixels** (mean 4.45, differing from row 1 by 83 grey levels where every other adjacent row pair
differs by ~6). Keeping it would feed the tracker a garbage scanline *and* shift the principal point
by a pixel against a calibration specified for 640x480. Frames are now cropped to 640x480.

## Why the VIO result is not usable

OpenVINS produced 455 poses covering only the last 18.2 s of 92.9 s, and diverged (95.6 m, and
196.3 m after tuning). The pose count was **identical across three configurations**, which rules out
tuning as the cause.

The cause is initialisation. OpenVINS static init estimates gyro and accel bias from a *stationary*
window and only then triggers on motion. This capture is moving from the first frame, so there is no
such window — measured, the lowest-motion windows are at t = 93.8-98.2 s, i.e. *after* the frames
end, when the headset was set down. It eventually initialised 74 s in on a marginal window and
diverged from there.

`notes/22`'s successful run (final ‖p‖ 0.203 m) had ~54 s of stillness before the motion. That was
accidental, and it was read at the time as an incidental detail rather than a precondition.

## Second attempt failed for a different reason, worth recording

The capture script was corrected to record a 20 s stationary lead-in and then prompt for motion. The
result was 105 s of complete stillness — Meta pose bounding box **2 mm x 1 mm x 1 mm**, IMU accel
std 0.0166 for the entire run.

The prompt was printed to command output, which the harness shows to the assistant and **not
reliably to the user**. There was no way for the user to see the cue to pick the headset up. Timing
for a user-in-the-loop capture has to be stated in the message *before* the capture starts; script
output is not a user-facing channel.

## What exists now

| piece | state |
|---|---|
| `tools/cam_tap/ibfs_hook9.c` | full-rate stereo capture, descriptor-driven, per-frame exposure timestamps |
| `tools/capture/cap9_run.sh` | frames + Meta poses + IMU, static lead-in, periodic prox re-assert |
| `tools/vio/build_euroc_leech.py` | dedupe, parity selection, stereo pairing, metadata-row crop, IMU clock offset |
| OpenVINS config for the (0,2) pair | generated, runs |
| **ATE / drift / RPE** | **still not delivered** |

Remaining: one capture that is **stationary for ~20 s and then moves for ~60 s**, with the timing
given to the user in advance. Everything downstream of that is built and validated.
