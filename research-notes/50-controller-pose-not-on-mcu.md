# Controller pose is not on the MCU stream — 2026-09-05

`notes/18` flagged the key architectural unknown for step 3: is 6DoF controller pose fused on the
MCU, in `trackingservice`, or from camera IR blobs? Answered as far as passive listening can answer
it, by capturing the full raw stream while both controllers were actually moved.

## The test

Every previous controller capture was **static** — controllers sitting still or held steady, so a
constant-valued pose field would be indistinguishable from a constant-valued anything-else. This
capture holds both controllers in view of the (stationary, desk-mounted) headset and waves them in
slow figure-eights for 60 s, specifically so that any transmitted pose would have to vary.

## Result: no packet type carries anything resembling a pose

Full inventory of the raw stream during the capture:

```
0x50  59,603 pkts   headset IMU
0x51   1,773 pkts   fixed ~29.6 Hz MCU tick (notes/45)
0x55   4,308 pkts   display vsync (notes/47)
0x8f  59,889 pkts   controller — both ids, ~499 Hz each
0xd9     120 pkts   periodic announce
0xe0   1,500 pkts   camera exposure
```

No new type appears. Everything controller-related is inside `0x8f`'s sub-records, all already
catalogued (`notes/49`): `0x41 0x82` (controller IMU, 18 B), `0x24` (buttons, 1 B), `0x63`
(trigger/grip, 3 B), `0x82` (thumbstick, 4 B), and `0x87` — the one field left unidentified.

**`0x87` is not a pose either**, examined now that it has real motion to show:

```
x: always >= 0, bounded ~0..13000, std ~4000     y: bounded ~-33000..0, std ~6000-9000
corr(x,y) = -0.57 to -0.69
```

Two bounded, anti-correlated scalars — consistent with a sensor pair such as capacitive
touch/proximity or an RF link metric, not a 3D position or orientation. A 6DoF pose needs at least a
translation vector and an orientation (quaternion or two vectors) — six to nine numbers with the
structure of smooth, unbounded motion. Nothing in the 4-byte record has that shape, and `0x87`
reports continuously through idle periods in every capture so far, which a pose field would not.

## What this settles, and what it does not

**Settled with fairly strong evidence:** controller pose is **not pre-computed and transmitted by
the controller's own MCU over the SyncBoss radio link**. The controller side of the link carries only
raw sensors — buttons, analogue triggers, thumbstick, IMU — never a fused position/orientation.

**Not settled:** whether fusion happens in `trackingservice` (IMU dead-reckoning, likely drift-
corrected by camera-observed IR LEDs) or is computed some other way. This test can only show what
crosses the wire we can read; it cannot see computation happening entirely inside `trackingservice`
from the controller IMU plus camera frames. That remains the leading hypothesis by elimination —
`notes/18` already noted `vendor.oculus.hardware.sensors@1.0::IControllerProvider` as the HAL seam,
and the camera path is now fully open (`notes/38`), so identifying IR blobs from the same frames
already being captured is the natural next step if 6DoF controller tracking is ever pursued.

**Consequence for step 3.** `notes/18`'s kill criterion — "if pose fusion is entirely inside
`libtrackingengines.so` with no usable intermediate, controller 6DoF becomes its own research
project" — is now the live scenario. Buttons and 3DoF (orientation from the controller's own IMU)
are fully open and decoded (`notes/49`). Full 6DoF position requires either reverse-engineering the
fusion inside `trackingservice`/`libtrackingengines.so`, or implementing constellation tracking from
the open camera frames independently. De-scoped from this session; recorded as the honest state
rather than left ambiguous.

## Housekeeping

Capture preserved at `exports/controller-pose-2026-09-05/ctl_wave.bin` (not committed — the `.bin`
convention excludes raw captures of this size; this note is the durable record). Device restored:
`prox_open`, SELinux Enforcing.
