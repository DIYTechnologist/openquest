# `components/controllers`

Decodes both Touch controllers (buttons, triggers, thumbstick, IMU) from the raw SyncBoss MCU
stream, with no Meta userspace code. Replaces the controller half of
`vendor.oculus.hardware.sensors@1.0` (`IControllerProvider`).

## Status

Input decoding is done — every control on both controllers, matched against Meta's reported state
(`research-notes/49`). **6DoF controller pose is not yet placed**: it is confirmed absent from the
raw MCU stream (`research-notes/50`), so it is fused either inside `trackingservice` from another
source, or from camera IR blobs (constellation tracking) — undetermined, and the open item for this
component.

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
- Controller 6DoF pose fusion location is open; if it turns out to require camera-based
  constellation tracking, that becomes its own research project (`research-notes/18` step 3 kill
  criterion) and this component's scope stays 3DoF + buttons until then.
