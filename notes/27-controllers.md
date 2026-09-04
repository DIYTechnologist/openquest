# Step 3 — controller stream found and partly decoded — 2026-09-04

`notes/01` rates controllers our **lowest-confidence** area and `notes/18` calls it the least-scoped
item with the highest chance of an unpleasant surprise. Status before today: nothing decoded, and
`tools/sb_survey` saw **no controller packets at all**.

## Why nothing was visible: every stream has its own enable

A camera probe alone yields no IMU and no controller data. Each stream is gated behind its own MCU
message, so finding a stream means finding its enable. Recovered by disassembling
`/system/vendor/lib64/libsyncboss.so` — the request/response helper takes the message type in `w1`:

| function | request | response |
|---|---|---|
| `syncboss_camera_probe` | **40** | 70 |
| `syncboss_camera_release` | **41** | 125 |
| `syncboss_camera_stop_streaming` | 45 | 125 |
| `syncboss_camera_deinit` | 47 | 125 |
| `syncboss_imu_enable` | **110** | 125 |
| `syncboss_mag_enable` | 116 | 125 |
| `syncboss_input_unpair` | 209 | 125 |
| `syncboss_input_start` | **213** | — |

The method is self-validating: it reproduces `imu_enable = 110 / response 125` (already known
independently) and `camera_probe = 40` / `camera_release = 41`, which match the **published GPL
driver** constants exactly.

Sending **type 213** turned on four packet types that had never appeared:

```
             camera probe only        + imu(110) + input_start(213)
0x46         1                        1
0x50         -                        24842  @993.7 Hz   IMU
0x51         -                          684  @ 29.5 Hz   camera exposure (the type sb_decode.py knew)
0x55         -                          944  @ 71.8 Hz   len 12
0x8f         -                        11278  @499.2 Hz   len 23..71  <-- CONTROLLER
0xd9         -                           21  @  1.0 Hz   len 33
0xe0         759                        759  @ 30.4 Hz   camera exposure stamp
```

## 0x8f is the controller stream

First 8 bytes are the device id, little-endian: `0143858a3ac372bd` reverses to
**`bd72c33a8a854301`**, exactly the id `trackinginterface_cli getcontrollerbuttondata` reports. Only
one controller appeared — the second (`6e59a754d9c4b837`) was asleep on the desk.

Layout is a **container of tagged sub-records** of variable total length (23–71 bytes):

```
0143858a3ac372bd            8-byte device id (LE)
0111011301000000000001000000 00   status header
4182 8bca40ed0000 bdff8d03c501 f0fff9ff0100   \  repeated sub-record
4182 58d240ed0000 c1ff8a03c501 f1fff9ff0100   /
2480 006380ffffff
```

### `0x4182` sub-record = controller IMU — confirmed

18 bytes: `u48 timestamp_us`, `int16[3] accel`, `int16[3] gyro`.

| evidence | value |
|---|---|
| rate from timestamp deltas | 1997 us -> **501 Hz** |
| accel magnitude, stationary | **1014.6**, tight range -> 1 g in **milli-g** |
| gyro means, stationary | (−15.6, −7.4, 0.0) -> near zero, as a still gyro must be |

### Container framing

Stripping the IMU sub-records out of every packet leaves a small set of tagged records, which makes
the framing legible: after the 8-byte id and a 14-byte header, records are
**`<tag> 0x80 <data[n]>`**, with `n` fixed per tag (the IMU record is the exception, `0x41 0x82` +
18 B):

| tag | size | observed behaviour | reading |
|---|---|---|---|
| `0x41` | 18 | 501 Hz, accel milli-g + gyro | **controller IMU — confirmed** |
| `0x24` | 1 | `0x10` idle; held at **`0x11`** and **`0x12`** during two different presses | digital buttons |
| `0x25` | 1 | 0 → 17 → **179** → **192** → 0, sustained at levels | **analogue** axis (trigger or grip) |
| `0x87` | 4 | 2x int16, e.g. (25404, 13826), (11307, 15617) | thumbstick / 2-axis |
| `0x82` | 4 | zero except one sustained window | event-specific, unidentified |
| `0x63` | 3 | `ffffff` mostly | unidentified |
| `0x26` | 1 | rare | unidentified |

`0xd9` (1 Hz, 33 B) embeds the same device id and looks like a periodic status/announce.

### `0x63` = two 12-bit analogue axes — trigger CONFIRMED

The 3-byte record packs **two 12-bit values, little-endian** (`v & 0xFFF`, `(v >> 12) & 0xFFF`), and
the scale is **inverted: 4095 = released, 0 = fully pressed**.

Confirmed against a controlled capture — a slow full-range squeeze, then three sharp presses one
second apart:

```
press events (axis A below 50%):
    0.0s ->   3.2s   (3178 ms)     <- initial squeeze and slow release
   24.1s ->  24.4s   ( 300 ms)  \
   24.9s ->  25.2s   ( 300 ms)   >  the three sharp presses, 0.8 s apart
   25.9s ->  26.2s   ( 317 ms)  /

24.07s   207     24.15s     0     24.39s  4095      <- full press and release in ~300 ms
```

172 distinct values on axis A over the full 0–4095 range: genuinely analogue, not a bit. **Axis A is
the trigger.** Axis B moved independently during a different part of the capture and is *probably*
the grip — plausible but **not confirmed**, since that press was not isolated.

### Remaining

| tag | status |
|---|---|
| `0x41` | **confirmed** — IMU, 501 Hz, accel milli-g |
| `0x63` A | **confirmed** — trigger, 12-bit, inverted |
| `0x63` B | probable grip — not isolated |
| `0x24` | digital buttons: `0x10` idle, `0x11`/`0x12` held. Which physical buttons is unknown |
| `0x25` | bitmask, values `0x80`/`0x88` — capacitive touch is the likely reading, unconfirmed |
| `0x87` | 2x int16, thumbstick-shaped — unconfirmed |
| `0x82`, `0x26` | constant throughout; unidentified |

Each remaining one needs the same treatment: **one control, isolated, with a handshake so the
capture window lines up with the action.** The method now works; it is repetition, not research.

## What this does and does not settle

**Settled:** controllers reach us with no Meta userspace code — enable type, stream type, device
identity, and a 500 Hz 6-axis IMU per controller.

**Not settled, and it needs a person:**
- **Buttons.** Nothing was pressed during the capture, so every button field is constant and cannot
  be located. Identifying them requires pressing each control while capturing — the acceptance
  criterion is 100 % agreement with Meta's reported state over >= 50 discrete events.
- **Pose.** The architectural question from `notes/18` task 4 is still open: whether 6DoF controller
  pose is fused on the MCU, in `trackingservice`, or from camera IR blobs. Nothing in `0x8f` looks
  like a pose yet — but a controller lying still on a desk is exactly the condition under which a
  pose field would be constant and invisible. `getcontrollertrackingdata` returned only
  `{"in_hand": false}` for both, which is consistent with pose only being produced when held.

Both are answered by the same worn session that step 2 needs.
