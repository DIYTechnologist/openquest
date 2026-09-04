# `/dev/syncboss_stream0` is not single-open — the step 2 IMU gap is closed — 2026-09-04

`notes/29` ended with one untested assumption blocking step 2: that `/dev/syncboss_stream0` is
single-open, so our VIO could not get IMU while the sensors HAL held the device — and the HAL must
keep running, or `trackingservice` has no poses to compare against. "Single-open" had been assumed
project-long and never checked.

**It is false.** The stream device is a broadcast fifo with per-client buffers. No interposition of
any kind is needed: we open it read-only alongside the HAL and receive the entire stream.

## Why, from the published driver

`syncboss_stream_open()` (`syncboss_spi.c:521`) performs **no exclusivity check whatsoever** — it
calls `miscfifo_fop_open()` and returns. That function (`miscfifo.c:167`) `kzalloc`s a *per-client*
struct, allocates that client its **own** `kfifo`, and `list_add`s it to `mf->clients.list`.
`miscfifo_send_buf()` (`miscfifo.c:210`) then walks the list and copies every packet into every
client's fifo. Multiple readers are the design, not a loophole.

Two properties make this a genuinely non-invasive leech, from the same source:

- The stream type filter is **per-file** (`miscfifo_fop_xchg_context` on our own fd), so our filter
  and the HAL's are independent. We set none.
- `should_send_stream_packet()` returns `true` when `context` is `NULL`, so an **unfiltered fd
  receives every type**. We see the HAL's traffic without altering what the HAL receives.

## Measured on device

`tools/sb_leech/` — opens `O_RDONLY`, polls, reads, decodes. It never writes to `/dev/syncboss0`,
sends no enable packets, issues no ioctl, and changes no MCU state.

Baseline: `vendor.oculus.hardware.sensors@1.0-service` (pid 769) held `/dev/syncboss_stream0` on
fd 16 throughout, and `trackingservice` (892) stayed up.

**Three simultaneous readers**, two passive leeches plus `sb_survey` (which generated the traffic by
opening an MCU session), all received the full stream:

| reader | bytes | 0x50 IMU | 0x51 cam-exposure | resync bytes |
|---|---|---|---|---|
| leech A | 851,235 | 19,874 @ 993.7 Hz | 590 @ 29.6 Hz | 0 |
| leech B | 851,403 | 19,878 @ 993.9 Hz | 590 @ 29.6 Hz | 0 |
| sb_survey | 596,904 | 13,936 @ 995.4 Hz | 414 @ 29.6 Hz | 0 |

Leeches A and B reported **byte-identical** first payloads (`9173e311...`, `65f3e311...`); the count
differences are start/stop timing over different windows. `dmesg` shows three separate
`SyncBoss stream handle opened` lines, one per client.

## Loss: none

The concern with a second reader is the per-client buffer: `SYNCBOSS_MISCFIFO_SIZE` is **1024
bytes** (`syncboss_spi.c:101`) — only ~24 packets, ~24 ms at this rate. A reader that stalls longer
than that loses data, and the driver logs `miscfifo ... is full`.

Measured from the IMU packets' own `u32 ts_us` (1 MHz nRF clock) over a 22.7 s capture, 22,553
packets:

```
median gap 1006 us -> 994.0 Hz
largest gap 1007 us          <- exactly one sample period
gaps > 1.5x median: 0
```

**Zero dropped samples.** The largest gap in the entire capture is one sample period, so nothing was
lost; polling and draining promptly is sufficient. Incidentally this measures the real IMU rate as
**994 Hz**, not the 1 kHz implied by `transaction_period_us = 1000`.

The second `u32` in the 0x50 payload is **not** a sequence counter — it was 0 for all 22,553 packets
(all id deltas 0). Loss must be detected from `ts_us` gaps, not from that field.

## Simultaneous with Meta's poses

`sb_leech` and `tools/pose_log/` ran together against the live stack: 22,553 IMU packets at 993.9 Hz
while `pose_log` read `TrackingServiceHeadTracker` from `trackingservice` pid 892. Both stamp
`CLOCK_MONOTONIC`, so with the leech's frames (`notes/29`) all three streams share one clock.

Meta reported **1 distinct pose** (0.1 Hz new poses) — expected, and the same condition as
`notes/29`: proximity uncovered means tracking is in standby. Only a worn session produces real
poses, and real pixels.

## Caveats worth carrying forward

1. **Starting our own MCU session pushes data into the HAL's fifo too.** `dmesg` recorded
   `miscfifo syncboss_stream0 opened by vendor.oculus.hardware.sensors@1.0-service (769) is full` —
   the HAL received packets it never asked for and did not drain. Harmless (kfifo simply drops), but
   in the real step 2 capture the *HAL* owns the session and will be draining, so this should not
   arise. Do not read it as us disturbing the HAL's own data.
2. One transient `Bad magic number detected: 0xcacacaca` / `SPI transaction rejected` at session
   teardown, out of millions of transactions (`num_bad_magic_numbers: 1`). Recorded for honesty; not
   pursued.
3. `0x46` (payload `0f`, one packet) appears once at session start and is not in the `notes/27`
   catalogue. Unidentified, low priority.

## State after the test

Device restored: all three Meta processes alive at their original PIDs (574, 769, 892), HAL still
holding its 4 syncboss fds, MCU back to duty-cycling (`awake 59 ms / asleep 51 ms`), scratch files
removed. `sb_survey`'s `camera_release` ran, and `dmesg` confirms `Turning off cameras`.

## What this changes for step 2

| piece | before | now |
|---|---|---|
| frames while `trackingservice` runs | works (leech) | works |
| Meta poses at >= 30 Hz | works (`pose_log`) | works |
| **IMU while the HAL runs** | **open — assumed impossible** | **works, lossless, no interposition** |
| common clock | yes | yes |
| real pixels + real poses | needs worn headset | needs worn headset |

Every piece of step 2's capture tooling now exists and has been demonstrated against the live stock
stack. The `LD_PRELOAD` read() tap on the HAL contemplated in `notes/29` is **not needed** and should
not be built. `tools/imu_intercept/` stays what it is — HIDL diagnostics, not a capture path.

**The only thing left for step 2 is the worn session** (⚠ `notes/18` track E): ≥ 2 minutes, with
translation and fast rotation, controllers paired and exercised so it serves step 3 as well, and
exposure `e3000_g160` rather than the saturating `e8000_g255` (`notes/15`).
