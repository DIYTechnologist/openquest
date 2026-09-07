# Controller tracker v1: bootstrapped LED model + working PnP tracking, from scratch — 2026-09-07

First working slice of a from-scratch controller constellation tracker (`research-notes/55`'s
"implementing our own replacement is still fully unstarted" item). Scoped per the approved plan to
v1 = offline, host-side, no LED geometry assumed or extracted from anywhere — bootstrapped from our
own stereo triangulation, since none exists in this project and `libtrackingengines.so` remains a
deliberate dead end (`research-notes/01`).

## Capture (Phase 1)

`/dev/video0` is still not freely available (`research-notes/55`'s undiagnosed respawn issue,
reconfirmed unchanged even after a full reboot). Used the existing `tools/cam_tap` leech instead
(`ts_ibfs9.sh` + `cap9_run.sh`): relaunches `trackingservice` itself with `ibfs_hook9.so` preloaded,
so it needs only `trackingservice` stopped (not the sensors HAL), and stayed on the same PID
throughout with no collision from whatever is now force-restarting other services.

**Reproduced `research-notes/41`'s open, undiagnosed truncation bug**: asked for a 53 s session
(8 s controller-still + 45 s controller-moving); `frames.idx` stopped growing after **~2.24 s** of
real frame coverage while poses kept flowing the whole time (3780 samples, full session) — so it is
specifically the image path that stops, not the whole capture. Confirmed it isn't the respawn
problem: the hooked `trackingservice` PID never changed. Still not diagnosed; noted again as an
open blocker, now hit twice independently.

Despite the short window, it captured real, visually-confirmed IR-LED blobs: a genuine **38 us**
exposure class (114 of 546 total frames) sits far below the two known SLAM classes (6498 us,
14003 us) — an even more extreme short exposure than `research-notes/41` characterised, consistent
with being the dedicated constellation exposure. Sample frames show a near-black background
(mean ≈ 4.2/255) with 2-8 sharp, saturated (255) points per frame, one of which clearly resolves the
controller's full LED ring shape. Preserved at
`exports/controller-constellation-2026-09-07/cap9/` (`frames.bin`/`frames.idx`/`meta_poses.csv`).

## Bootstrap (Phase 2) — `tools/controller_tracking/`

New, reused rather than reimplemented where possible: `blob_detect.py` (threshold +
`connectedComponentsWithStats`, intensity-weighted centroids — no blob code existed anywhere in
this repo before) and `bootstrap_model.py`, which reuses the KB4 unprojection math from
`tools/vio/rectify_pair.py`/`epipolar_check.py` verbatim rather than re-deriving it.

Method: pick the cam0/cam2 IR-class frame pair with the most simultaneous blobs (found: 8 vs 7,
16.5 us apart), stereo-match by epipolar-plane consistency (both rays already share the IMU frame
via each camera's own `T_imu_cam`, so no essential matrix is needed), triangulate matched pairs.

**First run found a real failure mode worth recording**: epipolar consistency alone accepted one
wrong correspondence — its ray-gap (5.2 mm) wasn't even the largest of the six matches, but its
triangulated depth was ~3x farther than the rest (602 mm from the median point). With several blobs
clustered in a small image region, a wrong pairing can still satisfy a tight angular epipolar
threshold. Added a median-absolute-deviation outlier filter (a rigid LED constellation must cluster
tightly; RANSAC over the whole match set would be the principled version, this was enough given how
starkly the one bad match stood out). Final model: **5 LED points**, spread 58.8 mm mean / 112 mm
max from centroid — physically plausible for a Touch controller's ring, at a plausible ~30-40 cm
hold distance from the camera.

## Tracking (Phase 3) — `pnp_track.py`

Per frame: detect blobs, brute-force search correspondences against the 5-point model (exact search
over permutations, not an approximation — small N makes this cheap, mirrors the `ConstBrute`/
`UnconstBrute` naming in `trackingservice`'s own log fields from `research-notes/55`), solve with
`cv2.solvePnP(..., SOLVEPNP_SQPNP)` on unit bearings treated as normalized-pinhole coordinates (same
trick `rectify_pair.py` uses downstream of KB4 unprojection — no distortion model needed a second
time). Output pose is composed through the camera's fixed `T_imu_cam` into the **head IMU frame**,
which does not move during this capture, so poses are comparable frame-to-frame without any
additional alignment step.

**Self-consistency check passed cleanly**: solving PnP on the exact frame the model was bootstrapped
from returns `t = (0.000, 0.000, 0.000)` — i.e. the whole bootstrap→track composition round-trips
correctly, not just "runs without crashing."

26/57 frames solved (reprojection error under threshold with ≥4 blobs). Two failure modes, both
honestly present in the output rather than filtered out:
- A few early frames (only 4 blobs visible) jump to physically impossible positions (~0.9 m in
  40 ms) — a wrong correspondence passing the reprojection-error threshold anyway, the same
  underlying risk the bootstrap step's outlier hit, not yet guarded against in the tracker itself
  (no temporal consistency / velocity check exists yet).
- A genuinely stable run of 15 consecutive frames (~600 ms, 7-8 blobs each) settles into a small,
  smooth trajectory: centroid `(-0.092, -0.009, +0.151)` m in the head frame, std
  `(15.7, 18.2, 6.9)` mm — a plausible amount of jitter for slow hand motion with no smoothing or
  IMU fusion, not obviously wrong.

## What this establishes, and what it doesn't yet

**Does establish**: the whole bootstrap-from-stereo-triangulation approach works end to end on real
hardware and real (if brief) data, self-consistently, without needing Meta's model or any external
geometry data. That was the load-bearing assumption of the whole plan and it held.

**Does not yet establish**: an accuracy number. Phase 4 (validate against Meta's own reported
controller pose, per the approved plan) is **not done** — `pose_log`/`meta_poses.csv` from this
capture is the **head** tracker (`/dev/ashmem/TrackingServiceHeadTracker`), not the controller.
Found the right target while investigating: `/dev/ashmem/TrackingServiceController` exists in
`trackingservice`'s own memory maps, structurally alongside `TrackingServiceHeadTracker` — the same
kind of shared-memory region `pose_log.c` already knows how to read, just pointed at the wrong one
for this purpose. Building a `pose_log`-style reader for it (same technique, new struct-offset
reverse engineering) is the concrete next step to close the loop with a real ATE number, the same
bar `research-notes/51`/`53` held VIO to.

## Open

- The frame-write truncation bug (~2.2 s instead of the requested window) — now hit on two
  independent sessions weeks apart, still not diagnosed. Blocks getting a longer, richer capture
  for a stronger Phase 4 validation set.
- No temporal/velocity-consistency rejection in `pnp_track.py` yet — the wrong-correspondence
  jumps are visible precisely because nothing currently screens them out.
- `TrackingServiceController`'s shared-memory layout is unknown; needs the same kind of probing
  `pose_log.c`'s header documents doing for the head tracker region.

## Housekeeping

New: `tools/controller_tracking/{blob_detect,bootstrap_model,pnp_track}.py`. Device restored after
the capture: `trackingservice`/sensors-HAL/`cameramuxmodeservice` all `running`, SELinux
`Enforcing`, `prox_open` sent — confirmed clean before ending the session.
