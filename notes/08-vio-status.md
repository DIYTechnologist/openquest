# Open VIO on Quest 1 — status (2026-09-01)

## ACHIEVED: end-to-end open VIO that tracks
Meta's closed blobs → open sensors → Basalt VIO trajectory, fully validated:
- **IMU**: raw `oculus_syncboss` kernel FIFO (`/dev/syncboss_stream0` type-0x50), 1 kHz,
  decoded to accel(g)/gyro(deg/s)/temp. No blob.
- **Cameras**: leech trackingservice's shared dmabufs (in-process tap; external read blocked by
  VM_PFNMAP). Clean 640×480 mono fisheye ×4 (hardware-synced exposures).
- **Time-sync**: cameras + IMU on one clock. FrameSet `w11` = precise exposure ts; RANSAC clock
  map `mono_ns = 982.343*nrf_us + b` (0.23 ms residual) unifies camera + IMU clocks.
- **Calibration**: factory Fisheye62 → KB4 + T_imu_cam (`tools/quest_calib_convert.py`).
- **Basalt** (built from source, `tools/basalt-docker/`): stereo-inertial VIO produces a real
  trajectory — 41 poses, no NaN, **0.378 m smooth path** (`exports/vio-precise/trajectory.png`).

## KEY LESSON
The ~10 ms poll-tap timestamp jitter was the sole convergence blocker (stereo pairs not truly
simultaneous → NaN/zeros). Precise `w11` exposure timestamps + sub-ms clock map → convergence.
Basalt is stereo-inertial (needs 2 cams; `keypoint_vio.cpp:303` triangulates from stereo).
**IMPORTANT clock note:** `w11` is ~6.2 s behind CLOCK_MONOTONIC; match pixel frames to the
FrameSet *host-read* time (aligned) to pick the frameset, then use its `w11` value.

## LIMIT (why the dataset is short: 45 pairs / 0.9 s)
The leech links 3 separately-captured streams (pixels, FrameSet w11, syncboss) by host time, which
is lossy — only a fraction of frames link cleanly with camId. Longer/denser needs the ATOMIC hook.

## NEXT (needs a fresh ~30-60 s worn+moving capture)
1. **Atomic hook**: `DualStreamHandle<FrameSet>::read()` returns OVR::Sensors::FrameSet with the
   ImageBuffers RESOLVED → {exposure ts, camId, pixels} together, no matching, all frames, correct
   camId. (RE: OVR::FrameSet layout + ImageBuffer pixel access in libvrsensors-hidlwrapper/
   libimagebuffer; sret return.) → full-length robust dataset.
2. Full trajectory + accuracy eval (drift), verify physical camera ids/extrinsics.
3. Live pipeline → Monado integration (real-time open tracking).
4. Blob-free endgame: drive msm-config camera session directly (drop the HAL).

Capture needs the headset WORN + moving (VIO tracks motion; frames only flow during active
tracking). One relaxed "look around the room" session suffices with the atomic hook.
