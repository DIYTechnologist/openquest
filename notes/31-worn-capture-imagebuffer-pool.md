# Worn capture: rig validated (r=0.997), but the leech cannot deliver sustained frames — 2026-09-04

First worn session (`notes/18` track E). Two results, one good and one that invalidates a documented
assumption:

1. **The capture rig is correct.** Meta's poses and our IMU were captured simultaneously for 160 s
   and agree to **r = 0.997**.
2. **The frame leech does not work for sustained capture.** It yielded 96 frames in 150 s, not the
   ~9000 expected, because `ImageBuffer` construction is a **pool allocation event**, not a
   per-frame event. Step 2's ATE number is therefore **not delivered** by this session.

## What was captured

| stream | result |
|---|---|
| Meta poses | **9559 distinct poses, 59.7 Hz, 160 s** — genuinely tracking, not standby |
| MCU IMU | **158,264 samples @ 993.6 Hz, 160 s**, 0 resync errors, no loss |
| FrameSet descriptors | **12,000 records @ 119 Hz** (= 4 cams x 30 Hz) |
| stereo frames | **96** (cam0=48, cam1=48) — needed ~9000 |

Meta's trajectory: 38.63 m path over 160 s, bounding box 0.84 x 0.94 x 1.30 m (the motion was
lean/rotate in place rather than walking), median speed 0.16 m/s, p95 0.75 m/s. Start-to-end
displacement 1.364 m against an intended return to the start point — but with Meta's own drift
unknown and unrefereed, that number cannot be attributed between real displacement and tracker
drift, so it is recorded, not interpreted.

## The rig is validated: r = 0.997

Independent cross-check of the two streams we *did* get, which share no code path: our raw syncboss
IMU decode versus angular speed differentiated from Meta's pose quaternions.

```
|gyro| from our decode : median 21.1 deg/s, max 531
Meta pose ang. speed   : median 20.8 deg/s, max 541
cross-correlation      : r = 0.997 at 0.07 s lag
```

This simultaneously confirms the IMU decode (scale and units), the shared-memory pose reader, that
both cover the same interval, and that the clock offset is recoverable to ~10 ms. Everything in the
step 2 pipeline is now proven **except the frames**.

Caveat: `sb_leech` writes raw stream bytes with no per-packet host stamp, so IMU timestamps are on
the nRF 1 MHz clock and the 0.07 s lag is a **clock offset recovered by correlation**, not a
measured latency. For the real capture the leech should host-stamp each read.

## Root cause: ImageBuffer is pooled, not per-frame

`notes/10` states camera pixels are "`ImageBuffer` objects reconstructed **per frame**". Measured
over 150 s, that is **false**. The ctor fires in four tight bursts and is otherwise silent:

```
FS reads (frame deliveries) : 12000 over 100.8 s = 119.0 Hz   <- frames flow continuously
IB ctor calls               :   768 over 151.1 s =   5.08 Hz  <- ctor almost never fires

cluster 0:  448 calls  t=  0.0..  0.6 s   (startup pool fill)
cluster 1:  128 calls  t= 83.0.. 83.0 s   (0.03 s)
cluster 2:   64 calls  t= 89.3.. 89.3 s   (0.02 s)
cluster 3:  128 calls  t=151.1..151.1 s   (0.06 s)
```

The ctor is a **pool (re)allocation**, after which the buffers are reused without reconstruction.
Every previous capture was <= 10 s and therefore sat entirely inside cluster 0, which is exactly why
this was never noticed — `notes/10`'s 576 frames were a pool fill, not a 576-frame stream.

This also overturns `notes/10`'s "sd/pixel VAs move every frame (walking mmap) -> a persistent-VA
poller would fail". With a pool the VAs are a small recycled set, so a persistent-VA reader is not
just viable, it is the correct design.

## The fix, already visible in this capture

The FrameSet descriptor carries what is needed, at the full 119 Hz:

```
w2  hi = 0..15  lo = 0      <- pool slot index, camId 0
w14 hi = 0..15  lo = 1      <- pool slot index, camId 1
w4  = 640 x 480             w11/w12/w13 = timestamps
```

Both `w2` and `w14` high dwords take exactly the values **0..15** — a 16-entry pool — and the
`ImageBuffer` `id` field observed at ctor time is likewise 0..15 with `cam = id & 3`. So:

1. Hook the ctor as now, but only to **learn `slot -> pixel VA`** (updating on each realloc burst).
2. On each `MessageQueue<FrameSet>::read()`, copy pixels from the slot VAs named by `w2`/`w14`, and
   stamp them with the FrameSet's own exposure timestamp.

That yields the full 30 Hz per camera, and as a side effect closes `notes/10`'s long-standing OPEN
THREAD (attaching a per-frame exposure timestamp), because the timestamp and the slot index arrive
in the same record. `notes/10`'s pairing problem was unsolvable only because it tried to correlate
the two hooks by host time; the slot index links them directly.

Risk to handle: reading a slot must not race the producer writing it. Copying immediately after
`read()` returns — when the frame is by definition ready — is the natural point.

## Not attempted: step 3

`trackinginterface_cli getcontrollerbuttondata` returns `{}` and takes **2.7 s per call**, far too
slow to score "100 % agreement over >= 50 events". This session therefore served step 2 only. The
raw syncboss stream was captured throughout and contains controller packets, so the data is banked,
but step 3 needs its own reference logger (a HIDL client, not the CLI).

## Housekeeping

- Session data pulled to `exports/step2-worn-2026-09-04/` **before** any restore, per the standing
  rule: `ib.log`, `ib.idx`, `ib_frames.bin` (29 MB), `imu.bin` (16 MB), `meta_poses.csv`.
- Device restored: `trackingservice` clean (no preload mapped), SELinux **Enforcing**.
- Before the session, `/data` writes were hanging — the UFS link death (gear 1/lane 1,
  `saved_uic_err=0x20`). `adb reboot` hung; recovered via sysrq. Link came back at **gear 3, lane 2,
  FAST MODE** and clock-gating/hibern8 were pinned off for the session. It did not recur.
- The "device is corrupt, press power" screen is Meta's `sysimgcheck` firing a `warning/verity_red`
  slideshow because `verifiedbootstate=orange` (unsigned boot image = our instrumented kernel). It
  is an attestation warning, **not** storage corruption; `/data` is clean ext4.

## Step 2 status

| criterion | state |
|---|---|
| Meta poses >= 30 Hz for >= 120 s | **done** — 59.7 Hz for 160 s |
| our VIO on frames from the same session | **blocked** — 48 stereo pairs, need ~4500 |
| ATE RMSE | **not delivered** |
| drift m/min | **not delivered** |
| RPE over 1 s windows | **not delivered** |

**Next:** build the slot-VA + FrameSet-triggered reader above, re-validate on the desk (the startup
burst fills the pool, so `slot -> VA` learning is testable without wearing anything), then **one
more worn session**. Everything else for step 2 is proven and does not need redoing.
