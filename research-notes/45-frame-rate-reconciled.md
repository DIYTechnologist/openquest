# Reconciling 25 vs 30 Hz: 0x51 is not the camera frame rate — 2026-09-05

Three numbers were in conflict: the leech sees ~50 Hz per camera (`notes/41`), `sb_survey` reports
type 0x51 "camera exposure" at 29.6 Hz, and the direct-kernel path was accepted at 30 Hz
(`notes/22`). Resolved by measurement, and one long-standing label turns out to be wrong.

## The cameras really do deliver 50 Hz under `trackingservice`

The decisive test is not timestamps but pixels. 600 consecutive cam0 frames from the motion capture,
MD5'd:

```
600 frames -> 600 distinct images (100 %), 0 exact duplicates, 0 adjacent identical pairs
span 11.98 s -> 50.07 Hz of genuinely distinct images
```

So the ~50 Hz is real content, not the same frame delivered twice with different stamps. Combined
with `notes/41` (two interleaved exposure classes 40 ms apart, offset ~17.8 ms) the picture is:
**2 exposure classes x 25 Hz = 50 Hz per camera**.

## 0x51 is a fixed ~30 Hz tick, unrelated to camera frames

Two independent observations kill the "0x51 = camera exposure @30Hz" label carried in
`tools/vio/sb_decode.py`:

1. **Phase.** During the capture, only 12 % of FrameSet stamps fall within 2 ms of an 0x51 event,
   and the median distance is 8.4 ms against a 33.8 ms period — which is exactly the median of a
   *uniform* distribution over half a period. The two event streams are uncorrelated in phase. A
   constant clock-offset error would shift every distance equally, not randomise them.
2. **Persistence when idle.** With tracking in standby, 0x51 continues at **29.6 Hz**, the same rate
   as during a full 50 Hz capture. A per-frame camera report cannot be rate-invariant to the camera.

## What the stream types actually are

Measured with tracking idle, and cross-checked against independent measurements:

| type | rate | reading |
|---|---|---|
| 0x50 | 993.7 Hz | headset IMU (established, `notes/30`) |
| 0x51 | 29.6 Hz | fixed MCU tick — **not** camera frames; rate-invariant to camera state |
| 0x55 | **71.9 Hz** | matches the independently measured display refresh of **71.819 Hz** (`notes/44`) — almost certainly the panel TE/vsync relayed by the MCU |
| 0xe0 | **25.1 Hz** | camera exposure stamps at exactly the per-exposure-class rate; `build_euroc_direct.py` already treats 0xe0 as the exposure stamp |

0x55 landing on the measured panel refresh, and 0xe0 landing on the measured per-class camera rate,
are two independent confirmations that were not used to derive either number.

## So where did "30 Hz" come from

Two separate sources, both explicable:

- **The 0x51 mislabel**, which put a 29.6 Hz number next to the word "camera" in `sb_decode.py`.
- **Our own configuration.** `cam_kernel` programs the sensors itself and runs them at 30 Hz
  (`notes/22`). That is a real 30 Hz — it is just *our* rate, not Meta's. `trackingservice` programs
  the same sensors for dual-exposure 2 x 25 Hz.

Both are correct for their own path. The error was treating one path's rate as a property of the
hardware.

## Consequences

- The leech VIO dataset is **25 Hz per camera** after parity selection, and that is the right rate —
  not a degraded 30 Hz.
- `sb_decode.py`'s 0x51 comment should be corrected; 0xe0 is the exposure stamp.
- 0x55 is a free, MCU-side vsync signal at panel rate. That is potentially useful for step 6A's
  motion-to-photon measurement: it gives a display-timing reference on the same clock as the IMU,
  without touching the display stack.
