# `components/controllers`

Decodes both Touch controllers (buttons, triggers, thumbstick, IMU) from the raw SyncBoss MCU
stream, with no Meta userspace code. Replaces the controller half of
`vendor.oculus.hardware.sensors@1.0` (`IControllerProvider`).

## Status

Input decoding is done — every control on both controllers, matched against Meta's reported state
(`research-notes/49`). **6DoF controller pose fusion is now understood, but not yet replaced.**
Confirmed absent from the raw MCU stream (`research-notes/50`), and now confirmed and quantified as
camera-based IR-LED constellation tracking, fused inside `trackingservice` itself
(`research-notes/55`, from the service's own logging: ~15-18 blobs/frame detected, ~5 matched to
the controller's known LED IDs, match success 0.95-1.00 when tracked). Implementing our own
constellation tracker to replace it is unstarted and is its own project
(`research-notes/18` step 3 kill criterion) — this component's actual scope is still 3DoF + buttons.

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
- 6DoF pose fusion does require camera-based constellation tracking (confirmed,
  `research-notes/55`), so it is its own research project per `research-notes/18` step 3's kill
  criterion, and this component's scope stays 3DoF + buttons until that project starts.
- Getting `/dev/video0` free for any future work here (e.g. capturing the raw blob images, not just
  trackingservice's summary stats) currently needs `trackingservice`/the sensors HAL stopped, and as
  of `research-notes/55` those no longer reliably stay stopped — open, undiagnosed.
