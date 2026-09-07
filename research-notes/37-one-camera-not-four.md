# We were tracking one camera, not four — 2026-09-05

The handheld motion capture answered the camera-identification question, but not the way the pixel
clustering was trying to. The clustering kept failing because the premise was wrong: **all 16
tracked buffers hold the same camera.**

## The measurement

`map7.log`, 8000 FrameSet records over ~45 s of hand motion, logging per-record the four camera
timestamps and a content hash of all 16 tracked buffers:

```
camera-timestamp advance pattern:  (1,1,1,1) x 2677     (0,0,0,0) x 5322
                                   -> all four cameras always advance together
framesets observed:                2677
slot change counts:                ~168 per slot, all 16 slots
total slot changes:                2688  ~=  2677
lag from frameset advance:         0, for every slot, ~100 % of the time
```

**Exactly one slot changes per frameset**, not four. 2677 framesets / 16 slots = 167 each, which is
what every slot shows. If our 16 mappings covered four cameras we would see ~4 changes per
frameset; we see one.

## What that means

The pool is **64 buffers**, not 16: 16 frameset ring indices x 4 cameras. `id` names the frameset
ring slot only (`notes/34`: all four descriptor blocks carry the same index and differ only in
`camId`), so the four ctor calls sharing an `id` are the four cameras — and `hook5`'s
`g_va[id]` table keeps whichever ran **last**, overwriting the other three.

Because the ctors for an id run in a fixed camera order, "last" is consistently the *same* camera.
So all 16 of our mappings are 16 framesets' worth of **one** camera.

This retro-explains three failed analyses, all of which were reading time differences as viewpoint
differences:

- the static 2x8 split (`notes/36`) — one camera at 16 different times, clustered by exposure drift;
- the temporal-signature correlation — uncorrelated because rounds are ~0.9 s apart under motion;
- the within-round clustering under motion — worse, not better, because faster motion makes the time
  gaps between buffers dominate.

More motion was never going to fix a mapping that only ever pointed at one camera.

## The fix, now precisely specified

Key the mapping table on the **pixel `native_handle` pointer** rather than on `id`, with room for
64 entries. Every distinct buffer then gets its own persistent mmap, and the four that change on
each frameset are that frameset's four cameras. Camera identity then follows directly from the
descriptor: the four blocks give `camId` and a per-camera capture timestamp in the same record, so
no pixel-content inference is needed at all.

That also means camera identification never actually required motion — it required tracking all 64
buffers. `notes/36`'s conclusion ("needs motion") was right about the data it had and wrong about
the cause.

## Captured and banked

The session is worth keeping regardless — it is the first capture with genuine motion:

| stream | result |
|---|---|
| Meta poses | **2700 samples, 60.0 Hz, all 2700 distinct** — real 6DOF motion, 0 read errors |
| IMU | 44,711 samples @ 993.6 Hz, 0 resync errors |
| frames | 30 rounds x 16 buffers x 640x481 = 141 MB |
| FrameSet records | 8000, with per-camera timestamps |

Exported to `exports/motion-2026-09-05/`, frames in `/tmp/rounds.tar` (141 MB, not committed per the
`exports/**/*.bin` convention).

Captured entirely over **wireless adb** (`adb tcpip 5555`, `192.168.2.71`), with root confirmed over
the WiFi transport — the USB tether is not required for capture, which removes the practical limit
on how far the headset can be carried.
