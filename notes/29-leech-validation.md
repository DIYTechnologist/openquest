# Leech revalidated; step 2 capture path assembled — 2026-09-04

`notes/28` established that our camera stack and Meta's tracker cannot run at the same time by the
obvious route (stopping the sensors HAL removes Meta's pose output entirely). Step 2's ground truth
therefore needs the **leech** — the `LD_PRELOAD` frame tap that reads frames *while*
`trackingservice` runs — exactly as `notes/18` originally specified.

## Still works, on the instrumented kernel

`tools/cam_tap/ibfs_hook.so` preloaded into `trackingservice` via `/data/local/tmp/ts_ibfs1.sh`:

```
trackingservice pid 17422, ibfs_hook mapped (4 entries)
448 IB lines in ~12 s
IB seq=0 host=61593727586862 id=0 640x481 fmt=1 px=7316b0d000 min=0 max=0 cam=0
IB seq=1 ...                 id=1 640x481                                cam=1
IB seq=2 ...                 id=2 640x481                                cam=2
```

Correct geometry, correct 4-camera round-robin (`cam = id & 3`), firing at the expected rate. The
mechanism survives the kernel change.

**`min=0 max=0`: the buffers are empty.** With the proximity sensor uncovered there is no active
tracking, so no real camera frames are produced — consistent with `notes/09`'s "frames flow only
while worn+moving". Nothing is wrong; real pixels need the headset on a head.

## The two tools coexist — which is the point

Running `tools/pose_log/` against the *preloaded* `trackingservice` at the same time:

```
[+] trackingservice pid=17422  TrackingServiceHeadTracker @ 0x7316e7a000 (8192 B)
[+] 90 samples in 3.00 s (30.0 Hz), 0 read errors
```

So frames and Meta's poses can be captured from one live `trackingservice` in a single session, and
**both are stamped on `CLOCK_MONOTONIC`** — the leech stamps each frame with host time at the
ImageBuffer ctor, and `pose_log` stamps each sample the same way. Time alignment for ATE is
therefore direct, and does not require solving `notes/10`'s open thread (attaching the per-frame
`FS` exposure timestamp), which matters for VIO accuracy but not for comparing two trajectories on a
shared host clock.

## The remaining gap: IMU while the HAL runs

Our VIO needs frames **and** IMU. The leech supplies frames. The IMU arrives on
`/dev/syncboss_stream0`, which the sensors HAL holds while it is running — and it must be running,
or Meta has no poses to compare against.

`tools/imu_intercept/` is **not** a solution: `hal_side_shim.cpp` and `imu_shim.cpp` are diagnostic
hooks from the HIDL investigation, not a capture tap.

**Untested assumption worth checking first:** that the stream device is genuinely single-open. The
driver uses `miscfifo`, which may support multiple readers; "single-open" has been assumed
throughout this project and never actually tested. If a second reader is allowed, step 2's IMU need
is solved outright with no new interposition. If not, the fallback is an `LD_PRELOAD` read() tap in
the HAL, mirroring the frame tap.

## Status

| piece | state |
|---|---|
| frames while `trackingservice` runs | **works** (leech, revalidated) |
| Meta poses at >= 30 Hz | **works** (`pose_log`, `notes/28`) |
| both simultaneously | **works** |
| common clock for alignment | **yes** — both `CLOCK_MONOTONIC` |
| IMU while the HAL runs | **open** — test the multiple-reader assumption first |
| real pixels | needs the headset worn |

## NOT COMMITTED

This note was written after the session's shell access stopped being available, so it is **not in
git** and neither is anything after commit `6cb2db3`. The device was left restored (clean
`trackingservice` with no preload, SELinux Enforcing, panel blanked) before that point.
