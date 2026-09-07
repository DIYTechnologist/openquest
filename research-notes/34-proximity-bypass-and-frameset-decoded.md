# Proximity bypass unblocks desk testing; the FrameSet descriptor is fully decoded — 2026-09-05

The user asked whether the proximity sensor could be covered. It can — but there is a **software**
bypass that is better, and it removes the constraint that has shaped this project's scheduling from
the beginning: that anything involving real frames or real poses needs a human wearing the headset.

## The bypass

```sh
adb shell 'su -c "am broadcast -a com.oculus.vrpowermanager.prox_close"'   # simulate worn
adb shell 'su -c "am broadcast -a com.oculus.vrpowermanager.prox_open"'    # restore
```

With the headset flat on a desk, `prox_close` produces full tracking:

```
Tracking Level: 6DOF (PT=1, PV=1, OT=1, OV=1)   Valid: Yes
StateMachine: "old_state":"Standby" -> "new_state":"Running"
Tracker:      "old_level":"3dof"    -> "new_level":"6dof"
Localizer:    loc_success true, 972 keypoints, 1910 unique points, num_images: 4
```

All four cameras, localiser running, poses valid. There is also a kernel-side route if the
broadcast ever stops working: `SYNCBOSS_PROX_DISABLE_MESSAGE_TYPE 204` in `syncboss_spi.c` is an MCU
message that disables prox.

## This invalidates `notes/33`'s second correction, written an hour earlier

`notes/33` concluded the frame-capture fix "cannot be validated on a desk" because
`MessageQueue<FrameSet>::read()` fired 12,000 times worn and **zero** times on a desk. The
observation was right; the inference was wrong. The gate is **not** a head — it is whether tracking
is *active*, and that is settable in software. Same hook, same desk, with `prox_close`:

```
before bypass: IB 448   FS 0
after  bypass: IB 576   FS 1746      (~145 Hz)
```

So the frame work **is** fully desk-testable and unattended after all. The correction stands only in
its narrow form: FS reads need active tracking, not a worn headset.

`notes/33`'s *first* correction is unaffected and still holds: the `ImageBuffer` `id` is a rolling
index, not a stable buffer handle (24-40 distinct pixel VAs per id).

## The 52-word FrameSet descriptor, fully decoded

Previously unobtainable — it needed active tracking. Structure is
**2 header + 4 image blocks x 12 words + 2 trailer = 52**:

```
w0        frame sequence counter (1844 in the sample)
w1        = 2

block k at word 2 + 12k, for camId 0..3:
  +0  hi = FrameSet buffer index (0..15)   lo = camId (0,1,2,3)
  +1  hi = 640   lo = 2            (width, format)
  +2  hi = 640   lo = 480          (width, height)
  +3  hi = 1     lo = 640          (?, stride)
  +4 +5 +6  zero
  +7  f64 exposure seconds         0x3f79ae92... = 6.36 ms
  +8  f64 gain                     0x401c000000000000 = 7.0
  +9  u64 ts  (CLOCK_MONOTONIC ns) common to all blocks
  +10 u64 ts  (CLOCK_MONOTONIC ns) common to all blocks
  +11 u64 ts  (CLOCK_MONOTONIC ns) DIFFERS PER BLOCK  <- per-camera capture time

w50       0xffffffff
w51       = 1
```

Two things fall out of this.

**All four blocks share one buffer index and differ only in camId.** So the index is per-FrameSet,
not per-camera — a ring of 16 framesets, each carrying 4 images. `notes/31` read the repeated index
as suspicious; it is simply correct.

**`notes/10`'s ONE OPEN THREAD is closed.** Word +11 of each block differs between blocks by tens of
microseconds and is already on `CLOCK_MONOTONIC` — a genuine **per-camera capture timestamp**,
delivered in the same record as the camId. The problem `notes/10` framed as unsolvable ("the IB ctor
stream and the FS read stream do not align 1:1 by host time") dissolves: the timestamp does not need
to be joined to the IB stream at all, because the FrameSet already names the camera it belongs to.
Exposure (6.36 ms) and gain (7.0) come free in the same block.

## What this changes

| previously blocked on a worn session | now |
|---|---|
| validating the frame-capture mechanism | **desk, unattended** |
| capturing 52-word descriptors | **done, desk** |
| per-frame exposure timestamps | **solved** — block word +11 |
| Meta poses at 6DOF for comparison | **desk, unattended** |
| a *moving* trajectory for ATE / drift | still needs a human — but **handheld, not worn** |

Only the last row still needs the user, and the requirement has dropped from "wear the headset for
2 minutes" to "pick it up and walk it around", which is a much smaller ask. A stationary desk
capture is also now possible unattended and is a legitimate measurement in its own right: a
correct VIO should report no motion, so stationary drift is directly measurable.

## Device state

Restored: `prox_open` sent, `trackingservice` clean with no preload, SELinux Enforcing.
