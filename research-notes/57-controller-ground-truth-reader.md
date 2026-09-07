# Controller ground-truth reader: TrackingServiceController read and validated — 2026-09-07

`research-notes/56`'s open item — no controller ground truth existed to validate the from-scratch
tracker against, since `pose_log` only reads the head tracker. Closed: `TrackingServiceController`
(noted as a candidate location since `research-notes/19`/`24`, never actually read) is now read,
its layout mapped, and a working logger built and cross-validated against `dumpsys tracking` to
full precision.

## Injection was tried first, and hit a real dead end — correctly, not by giving up early

The plan was to repeat `research-notes/28`'s method: inject a known, distinctive pose via
`updateRemotePoseField` (transaction code 1, confirmed in `research-notes/23`), then search memory
for it. `remote_pose_probe.c` implements the call. It was rejected every time
(`accepted=false`, no exception) — including with the target controller actively powered on,
paired, and 6DOF-tracked, and with proximity bypassed. Before accepting that as a dead end, the
actual proxy was disassembled (`libossdk.oculus.so`,
`BpTrackingDataInjectionService::updateRemotePoseField`) to check the implementation against the
real binary rather than trust the signature alone: transaction code 1, parcel order
`writeInterfaceToken → writeString16(id) → writeInt32(field) → writeFloatVector(values)`, `flags=0`
— bit-for-bit what `remote_pose_probe.c` sends. The rejection is therefore a server-side decision
inside `trackingservice`/`libtrackingengines.so`, which `research-notes/01` already ruled off
limits (25 MB stripped monolith, deliberate dead end). Correctly stopped there rather than going
further into that binary.

## Reading the controller's own live pose instead

No injection needed: the controller was already being tracked. Read a `dumpsys tracking` value
(2-3 s latency, 2 decimal digits of precision — not enough for an exact byte match) alongside a
full raw dump of the 16 KB `TrackingServiceController` region (`mempeek dump`), and matched by
tolerance rather than exact bytes. Found it immediately: position within 0.05 m of the dumpsys
reading appeared at 34 offsets, evenly spaced (mostly) 0xa8 bytes apart, with a matching quaternion
(exact to the printed precision) 0x10 bytes before each — the same relative per-slot layout as
`TrackingServiceHeadTracker` (`research-notes/28`: quat at slot+0x10, pos at slot+0x20), just a
different, larger ring (32 slots vs 2) and a wider stride (0xa8 vs 0xa0 — presumably more trailing
fields per slot; not decoded).

## Two real bugs in the auto-detection logic, both instructive

Building `controller_pose_log.c` to auto-detect this ring at startup (so it doesn't depend on a
hardcoded offset that could drift) hit two distinct bugs, each of which produced a plausible-looking
wrong answer rather than an obvious crash:

1. **Scanning every 4-byte offset, tracking one running chain.** A single spurious "looks valid"
   reading at the *wrong* phase, sitting between two genuinely valid slots, broke the chain even
   though the real slots either side were fine. Confirmed directly: the true slots at 0x1f94,
   0x203c, 0x20e4, ... were each individually valid with exact 0xa8 spacing, yet the single-chain
   scan reported a best run of 1. Fixed by building a full byte-indexed candidate SET first, then
   checking chain membership by direct offset lookup (`s`, `s+STRIDE`, `s+2*STRIDE`, ...) rather
   than by whether the immediately-preceding scan step happened to continue a chain.
2. **Unit-quaternion-norm alone is too weak a filter.** `(0, 0, 0, 1)` — the identity quaternion —
   has norm exactly 1.0 and occurs constantly in zero-initialized or padding memory. This produced
   a confident, structurally-plausible 33-slot "run" at a materially different (wrong) offset, with
   every position reading `(0, 0, 0)`. Fixed by also requiring the co-located position to be
   nonzero and within a physically plausible range (<2 m) — trivial to add, and the kind of check
   that's easy to skip when the first filter already looks sufficient.

Neither bug was silent in the sense of "wrong but undetected forever" — both were caught by cross-
checking against data already independently verified (the manually-confirmed offsets, and physical
plausibility) rather than trusting the tool's own confident output, the same discipline
`research-notes/28` named explicitly: "a negative result from an instrument that has never
produced a positive is not evidence." Here it was a confident *positive* that needed the same
scrutiny.

## Validated

```
dumpsys tracking:      rot=(0.41, 0.47, 0.77, -0.15)   trans=(-0.24, -0.01, -0.20)
controller_pose_log:   quat=(0.4128, 0.4656, 0.7690, -0.1464)  pos=(-0.2441, -0.0146, -0.2021)
```

Exact to dumpsys's printed precision, sampled simultaneously. Rate varies with actual motion
(11-30 Hz observed) since freshness is detected by diffing consecutive polls rather than a decoded
sequence counter — a real slot that doesn't change between polls doesn't generate a sample, so the
reported rate is a floor on the true update rate, not a ceiling.

## What this enables, still not done

This closes the ground-truth gap for validating `research-notes/56`'s tracker with a real ATE
number: a future capture needs `tools/cam_tap`'s leech (images) and this reader running
simultaneously, alongside `sb_leech` (IMU) as already done for step 2's head-pose validation
(`research-notes/51`). Not done this session — needs a new capture, and the frame-capture
truncation bug (`research-notes/56`, `research-notes/41`) still caps how much usable data such a
capture would contain regardless.

## Housekeeping

New: `tools/controller_tracking/remote_pose_probe.c` (the injection attempt, kept — it's correct
and reusable if the server-side rejection reason is ever found some other way) and
`controller_pose_log.c` (the working reader). Both throwaway/bring-up tools relative to
`components/tracking`'s product code, same tier as `tools/mempeek` and `tools/pose_log`. Device
restored: `trackingservice`/sensors-HAL/`cameramuxmodeservice` all `running`, SELinux `Enforcing`,
`prox_open` sent, all test binaries removed from `/data/local/tmp`.
