# `components/controllers`

Decodes both Touch controllers (buttons, triggers, thumbstick, IMU) from the raw SyncBoss MCU
stream, with no Meta userspace code. Replaces the controller half of
`vendor.oculus.hardware.sensors@1.0` (`IControllerProvider`).

## Status

Input decoding is done — every control on both controllers, matched against Meta's reported state
(`research-notes/49`). **6DoF controller pose: a from-scratch v1 tracker exists and works,
end to end, on real data** (`research-notes/56`) — not yet a product-quality replacement. Confirmed
absent from the raw MCU stream (`research-notes/50`); confirmed and quantified as camera-based
IR-LED constellation tracking fused inside `trackingservice` itself (`research-notes/55`). Since no
LED geometry model exists or is extractable anywhere in this project, `tools/controller_tracking/`
bootstraps one from our own stereo triangulation (no Meta data used) and tracks per-frame pose
against it (`research-notes/56`: brute-force correspondence search; `research-notes/59`: a faster,
temporal prior-guided search that also self-disambiguates most wrong matches once tracking is
established). **When correspondence is correct, accuracy is sub-centimetre** (1.8-3.7 mm median,
robust-fit) **against the controller's own live tracked pose as ground truth**
(`research-notes/57`) — **but correspondence is only correct in ~80% of frames** at the current
tuned thresholds (`research-notes/60`; was ~73% before tuning, `research-notes/59`); the remainder
are outright wrong-correspondence outliers, not small errors. Both halves of that matter — quoting
either alone misrepresents it. This tooling lives in `tools/`, not here, until it's proven — this
component's own scope is still 3DoF + buttons.

## What it does

- `sb_leech` opens `/dev/syncboss_stream0` **read-only** and passively copies every packet to a
  file. This is safe because the driver's fifo is multi-reader by design, not single-open
  (`research-notes/30`) — no write, no ioctl, no MCU state change, so it can run even while Meta's
  own stack holds the device.
- `ctl_decode.py` decodes the capture on the host. Framing:
  ```
  01 03 00 <type> 00 <len> <payload>          outer syncboss framing
  type 0x8f = controller, ~500 Hz per controller

  payload[0:8]    device id
  payload[8:23]   15-byte header
  payload[23:]    tagged records: <tag> 0x80 <data[n]>, n fixed per tag,
                   except the IMU record (0x41 0x82 + 18 bytes)
  ```
  Record sizes and button bits: `0x24` = buttons (`0x01` primary, `0x02` secondary, `0x04` stick
  click, `0x08` menu/special), `0x63` = twin 12-bit inverted trigger/grip axes, `0x82` = thumbstick
  (2×int16). **Handedness is read from the wire** (`byte10` of the `0x8f` header), never from the
  device id — ids are per-unit and differ on every headset (`research-notes/49`).

## Build & run

```
make -C components/controllers          # -> components/controllers/sb_leech (aarch64)
adb push components/controllers/sb_leech components/controllers/ctl_run.sh /data/local/tmp/
adb shell sh /data/local/tmp/ctl_run.sh 240 /data/local/tmp/ctl_capture.bin
adb pull /data/local/tmp/ctl_capture.bin
python3 components/controllers/ctl_decode.py ctl_capture.bin
```

## Known limits

- No build step for `ctl_decode.py`/`ctl_run.sh` — pure Python/shell, run as-is.
- **~20% of frames are still wrong-correspondence outliers**, not small errors (up to 0.68 m off) —
  tightening the prior-guided path's gate/reprojection thresholds cut this from ~27%
  (`research-notes/60`), but no further gain was found before tightening became pathological (the
  brute-force fallback dominates when the prior almost never satisfies its own gate). Any blob
  detector alone cannot distinguish the tracked controller's real LEDs from another genuine IR
  source in view (the other controller, sitting idle, was found contaminating every frame,
  `research-notes/59`) — only motion does, since the camera is fixed for the whole capture; this is
  filtered (`find_static_positions`/`filter_static` in `blob_detect.py`) but not perfectly. Not
  real-time, not on-device.
- Sim3 alignment needs scale ~0.34 to fit, consistently across three different bug-fix/tuning rounds
  — verified this is *not* a triangulation math bug (a synthetic round-trip test recovers a known 3D
  point to 0.0000 mm). One concrete hypothesis (a lever-arm/reference-point mismatch between our
  model's centroid and Meta's own convention) was checked and did not hold up cleanly across
  different subsets of the same data (`research-notes/60`) — still open.
- Getting `/dev/video0` free for a longer/cleaner capture currently needs `trackingservice`/the
  sensors HAL stopped, and as of `research-notes/55` those no longer reliably stay stopped — open,
  undiagnosed, hit again in `research-notes/56`/`58` via a different capture path.
- `updateRemotePoseField` (controller pose injection) is verified bit-for-bit correct against the
  real proxy disassembly but is rejected by `trackingservice` regardless of controller state
  (`research-notes/57`) — a server-side precondition inside code this project has already ruled off
  limits (`research-notes/01`). Not pursued further; the ground-truth reader uses the controller's
  own live tracked pose instead, which doesn't need injection at all.
