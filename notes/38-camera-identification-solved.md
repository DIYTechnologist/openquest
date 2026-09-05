# Camera identification solved — all four, from the ctor, no pixel inference — 2026-09-05

`notes/37` predicted the pool is 64 buffers and that keying the table on the pixel `native_handle`
would expose all four cameras. Both confirmed, and the identification is exact.

## Measured

`tools/cam_tap/ibfs_hook8.c` keys on the handle pointer (96 slots of headroom) and records `k`, the
position of each ctor within its `id`'s run of four:

```
MAP entries created : 64        <- exactly the predicted 16 framesets x 4 cameras
REBIND events       :  0        <- (id,k) is stable per buffer for its whole life
entries with >1 k   :  0 / 64   <- k never changes for a given buffer
```

The partition is perfectly clean, 16 buffers per camera:

```
k=0 -> entries  0..15      k=2 -> entries 32..47
k=1 -> entries 16..31      k=3 -> entries 48..63
```

And per frameset, the set of buffers whose pixels changed:

```
change-sets of size 4                     : 1740
of those, k exactly {0,1,2,3}             : 1734   (99.7 %)
```

So each frameset writes exactly one buffer per camera, and `k` identifies which. **`k` is the
camera index**, recoverable at ctor time with no pixel-content inference at all.

(Sets smaller than 4 — 1182 of 3030 — are the content hash missing an unchanged 64-byte sample on a
near-static scene, not missing writes. That is a detection limit of the probe, not of the mapping;
whenever all four are detected they are one per camera 99.7 % of the time.)

## Visual confirmation

Dumping one buffer per `k` at a single instant gives four genuinely different views of the room —
different walls, different ceiling/desk framing, visible parallax. Pairwise normalised distance:

```
        cam0  cam1  cam2  cam3
cam0    0.00  0.95  0.70  0.85
cam1    0.95  0.00  0.89  0.88
cam2    0.70  0.89  0.00  0.88
cam3    0.85  0.88  0.88  0.00
```

**0.70-0.95**, against **0.05-0.16** when all tracked buffers were the same camera (`notes/36`). An
order-of-magnitude separation, and it is visible by eye. Frames exported as PNGs in
`exports/camera-ident-2026-09-05/`.

## What this retires

- `notes/10`'s `cam = id & 3` — wrong; `id` is the frameset ring index.
- `notes/31`'s pool-slot theory and `notes/33`'s "most recent VA per id" — both chased a mapping
  that pointed at one camera.
- `notes/36`'s "camera identification needs motion" — it never did. It needed all 64 buffers. The
  handheld capture was not required for this, though it produced a good motion dataset anyway
  (`notes/37`).

The general lesson, having got this wrong four times: every failed attempt inferred identity from
pixel content, and pixel content conflates viewpoint with time. The descriptor and the allocation
order carried the answer directly the whole time.

## Remaining before a dataset

`k` gives a *stable, distinct* camera index but does not by itself prove `k == n` in
`camera_calibration_v2.json`'s ordering. Intrinsics and extrinsics are per-camera, so the four must
be matched to the calibration before a metric dataset is possible. That is offline work: compare the
observed inter-camera geometry against the calibrated `DeviceFromCamera` extrinsics and take the
assignment that fits. The descriptor's `camId` field (`notes/34`) is the other half of the check —
correlating `k` against `camId` per frameset should pin it directly.

## Device

Restored: `prox_open`, `trackingservice` clean, SELinux Enforcing.
**UFS soak on the stock kernel: 6 h 53 m, 0 reset/UIC errors**, across many service restarts, ~300 MB
of dumps and a full motion capture. The instrumented kernel wedged twice inside ~15 minutes of
comparable load. That is now strong evidence our instrumented build caused it.
