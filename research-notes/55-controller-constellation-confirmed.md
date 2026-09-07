# Controller 6DoF is camera-based constellation tracking, inside trackingservice — quantified — 2026-09-07

`research-notes/50` narrowed controller pose fusion to "trackingservice, likely IMU dead-reckoning
drift-corrected by camera-observed IR LEDs" but could not settle it — a passive listener on the
SyncBoss wire cannot see computation happening entirely inside `trackingservice`. Settled now, by a
different passive method: `trackingservice`'s own logging, at its default verbosity, says so
directly and with numbers.

## How this was found

Setting up the capture `research-notes/52` called for next (camera frames with a controller in
view, to search the discarded dim/short-exposure class for IR blobs — `research-notes/41`) hit an
operational snag: getting `/dev/video0` free for `cam_kernel` needs both `trackingservice` and the
vendor sensors HAL (`vendor.oculus.sensors-hal-1-0`) stopped, and — unlike what
`tools/cam_kernel/run_ioctl_trace.sh` assumes — **both now respawn within 1-3 s of `stop`, even
immediately after a clean reboot.** Not diagnosed (see Open, below); a forceful `pkill -9` was
available but not attempted without checking in first, since it's a step up in how hard the state
is to cleanly reverse.

The respawn's own log burst turned out to be more useful than the capture it was blocking. It
showed `trackingservice` (pid, tag `[CT]`) logging a constellation LED-matching model on startup.
That prompted a **fully passive** follow-up, no service manipulation at all: proximity-bypass
(`research-notes/34`) to get the head into 6DOF/RUNNING, then a plain `adb logcat` while a
controller was powered on and moved in slow figure-eights for 45 s.

## What the logs say, directly

```
$ ps -p 6615
system  6615  1  ...  trackingservice
```

The `[CT]` / `CONSTELLATION` log lines come from `trackingservice` itself, not a separate process.

Per-controller, per-~2s interval (`CONSTELLATION:VISION_STATUS`), for the controller that was
actually moved (`bd72c33a8a854301`, right):

```
BlobDetection: #BlobsDetected = 14.8 - 18.1 (stat A), 30.7 - 37.4 (stat B)
Matching:      MatchSuccess 0.948 - 1.000
               #MatchedBlobs = 11.0 - 15.3 (A), 34.0 - 38.6 (B)
               #MatchedLeds  = 4.73 - 5.48 (A), 1.14 - 2.44 (B)
MatchMethod:   Prox 0.94-1.00, ConstBrute 0.000-0.008, UnconstBrute 0.000-0.004
Fusion:        ImuFromCamMs = (5.0-5.6, 0.4-7.9)
FusionStatus:  HasPosition 0.952-1.000, HasVelocity 0.948-1.000
```

The controller left stationary/out of view (`6e59a754d9c4b837`, left) shows the clean negative
control throughout the same window: `MatchSuccess=0.000`, `#MatchedBlobs=(0,0)`,
`HasPosition=0.000` — absence, not noise, which is exactly what "not currently in view" should
look like if this is genuinely camera-based.

Also present in the raw stream (not yet reduced to numbers): `CONSTELLATION: R: Camera is ahead of
IMU by 0.7 ms! Sleeping vision thread for 3 ms` — a real-time scheduling detail suggesting the
vision and IMU threads are explicitly paced against each other, and
`TrackingService: Attempted to control leds, but gatekeeper is disabled. Skipping operation` — the
controller LED on-time (strobe) is itself software-controlled, gated by something not enabled in
this state.

Raw capture (1877 lines, 45 s window) and the extracted `CONSTELLATION` subset (87 lines) are at
`exports/controller-constellation-2026-09-07/`.

## What this settles

**Controller 6DoF pose is fused inside `trackingservice`, from camera-observed IR-LED blobs
matched against a known controller LED constellation model, fused with the controller's own IMU.**
Not on the MCU (`research-notes/50`, unchanged), not in a separate process, and it is a real,
currently-working pipeline on this device — `MatchSuccess` at 0.95-1.00 and `HasPosition=1.000` for
several consecutive samples is a live, functioning tracker, not a stub.

The matcher has (at least) two modes, both visible in `MatchMethod`: **proximity-based** (matching
blobs to their previous frame's position, `Prox≈0.94-1.00` when tracking is already locked) and
**brute-force** (`ConstBrute`/`UnconstBrute`, small nonzero fractions — the re-acquisition path when
proximity matching can't find a blob, e.g. after occlusion or fast motion). This matches the
`BruteMatching` model-pair/model-triangle precomputation seen at trackingservice startup in the
earlier (accidental) respawn log capture.

**This also answers `research-notes/18` step 3's last open item** — "if camera-based: quantified —
how many IR blobs per frame" — without needing the planned dim-frame capture at all:
**`#BlobsDetected` ≈ 15-18 per frame, of which ≈ 5 are matched to the controller's own known LED
IDs** (`#MatchedLeds`). The dim-frame capture (`CAMKERNEL_SAVE_THRESH=0`, added to `cam_kernel` this
session) is no longer necessary for this question, though it remains available for anyone who wants
to see the actual blob images rather than trackingservice's own summary statistics.

## Open

- **Why `stop trackingservice`/`stop vendor.oculus.sensors-hal-1-0` no longer stick**, even from a
  clean reboot, unlike what the existing capture scripts assume. Something restarts both within
  1-3 s. Not diagnosed here — this blocks any future `cam_kernel`/`cam_direct` capture until either
  found or worked around (e.g. `pkill -9`, not yet tried). Worth investigating before the next
  camera-path session, since every existing capture script assumes the old behaviour.
- LED gatekeeper ("Attempted to control leds, but gatekeeper is disabled") — what enables it, and
  whether it matters for anything this project does.
- The `(stat A, stat B)` pairing in `#BlobsDetected`/`#MatchedBlobs`/`#MatchedLeds`/`ImuFromCamMs`
  is unlabelled in the log line itself — read here as two different statistics (plausibly mean and
  a second moment, or two separate cameras) rather than decoded from source. Worth confirming before
  quoting the numbers above as anything more precise than "order of magnitude, both eyes agree".

## Housekeeping

`components/camera/src/cam_kernel.c` gained `CAMKERNEL_SAVE_THRESH` (env var, default 20.0, set to
0 to keep the dim exposure class) while this session's original plan was still the dim-frame
capture. Kept even though not needed for this finding — real, reusable, harmless.

Device restored: `prox_open` sent, `trackingservice`/`sensors-hal`/`cameramuxmodeservice` all
`running`, SELinux `Enforcing`. No lasting device-state changes from this session.
