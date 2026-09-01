# Quest 1 mono-inertial VIO dataset (2026-08-31)

Captured by leeching trackingservice (LD_PRELOAD tap) while the HMD tracked, plus the open
syncboss kernel FIFO for hardware-synced timestamps. EuRoC/Basalt layout.

- `mav0/cam0/data/<t_ns>.png` — 261 frames, 640x480 8-bit mono, ~23 fps (one tracking camera).
- `mav0/cam0/data.csv` — `#t[ns],filename`.
- `mav0/imu0/data.csv` — `#t[ns],wx,wy,wz,ax,ay,az` (SI: rad/s, m/s^2), ICM-20602 @ ~1 kHz.
- `basalt_calib_mono.json` — KB4 intrinsics (approx of Meta Fisheye62) + T_imu_cam + IMU noise.

## Provenance / caveats
- Timestamps: camera exposure ts (syncboss type-0x51) and IMU ts (type-0x50) are BOTH the nRF
  1 MHz clock -> hardware time-synced, offset ~0.
- Frame->exposure association: monotonic snap to the 30 Hz exposure grid (poll jitter ~10 ms);
  a small fraction may be off-by-one.
- Physical camera identity of the captured stream is UNVERIFIED (calib uses cam0 as first approx;
  may need cam1/2/3 extrinsics).
- Bring-up artifact (leeches Meta's trackingservice), not a blob-free capture.

## Basalt run result (2026-09-01)
Built Basalt from source (tools/basalt-docker/, pre-vcpkg commit + modern cmake) and ran
`basalt_vio` mono-inertial on this dataset. Outcome: **loads + runs end-to-end but produces
0 poses** — Basalt's keypoint VIO triangulates its landmark map FROM STEREO
(keypoint_vio.cpp:303 "Triangulate new points from stereo"), so a single camera never builds a
map and the estimator never initializes. **Basalt is stereo-inertial; this dataset is mono.**

To actually track with Basalt we need a STEREO pair: two of the 4 (hardware-synced) tracking
cams with known baseline (from the factory calib) + clean per-frame association for both.
Alternatively use a mono-capable estimator (OpenVINS / VINS-Mono).

Pipeline proven working: sensor capture (open) -> hardware-synced timestamps -> EuRoC dataset ->
Basalt loads calib+images+imu and runs. The gap is data (mono + best-effort assoc), not plumbing.
