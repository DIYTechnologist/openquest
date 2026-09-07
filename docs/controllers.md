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
against it via brute-force correspondence search + PnP — validated self-consistent on a real
capture, not yet validated for accuracy. The missing ground-truth reader now exists and is
validated (`research-notes/57`: `TrackingServiceController` shared memory, cross-checked exact to
`dumpsys`'s precision) — closing the loop with a real ATE number needs a new capture running it
alongside the image capture, not done yet. This tooling lives in `tools/`, not here, until it's
proven — this component's own scope is still 3DoF + buttons.

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
- The v1 tracker (`tools/controller_tracking/`, `research-notes/56`) has no accuracy number yet, no
  temporal/velocity consistency check (a wrong blob correspondence can still pass its reprojection
  threshold and produce a physically impossible frame-to-frame jump — observed directly), and only
  ran on a ~2.2 s capture window. It is not real-time and not on-device.
- Getting `/dev/video0` free for a longer/cleaner capture currently needs `trackingservice`/the
  sensors HAL stopped, and as of `research-notes/55` those no longer reliably stay stopped — open,
  undiagnosed, hit again in `research-notes/56` via a different capture path.
- `updateRemotePoseField` (controller pose injection) is verified bit-for-bit correct against the
  real proxy disassembly but is rejected by `trackingservice` regardless of controller state
  (`research-notes/57`) — a server-side precondition inside code this project has already ruled off
  limits (`research-notes/01`). Not pursued further; the ground-truth reader uses the controller's
  own live tracked pose instead, which doesn't need injection at all.
