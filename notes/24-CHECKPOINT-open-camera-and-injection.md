# CHECKPOINT — open camera path done, pose injection proven — 2026-09-04

Read this first on resume. Supersedes the "next steps" of `notes/13` and `notes/10`; their standing
rules and traps still hold.

**Project goal:** replace Meta's blobs on an EOL Quest 1 (`monterey`, msm8998) with an open VR
stack, so the device can run a modern OS and general APKs (Virtual Desktop, ALVR/SteamVR).
Strategy (`notes/17`, `notes/18`): replace Meta's services **one at a time on the stock OS**, so the
eventual OS swap is a *port of known-working code* with a known-good fallback.

---

## Scoreboard

| # | Step | State |
|---|---|---|
| 0 | Open VIO converges | **DONE** — 0.56 m on a 23 s capture (`notes/14`) |
| X | Real-time budget on-device | **DONE** — 31.75 ms/frame tuned vs 33.3 ms budget (`notes/20`) |
| 1 | Direct-kernel camera (B2) | **DONE — 5/5 criteria** (`notes/22`) |
| 4 | `trackingservice` in place | **task 1 DONE** — pose injection works (`notes/23`) |
| 2 | Ground truth vs Meta | schema confirmed; **logger blocked on shared-memory path** |
| 3 | Controllers | both visible; stream survey done; decode not started |
| 5 | OS swap | not started (gated on 1, 3, 4) |

## What is now true that was not

**The camera path is fully open, sensor to 6DoF.** `tools/cam_kernel/` drives all four tracking
cameras with **zero Meta userspace blobs** (verified in-process: 0 Meta libs mapped during capture),
and a dataset captured through it drives OpenVINS to **final ‖p‖ 0.203 m** over a 7.24 m path. The
step-0 baseline of 0.56 m went through *B1*, i.e. Meta's blobs — so there is no regression.

**Meta's own tracker will report our poses.** `TrackingDataInjection` transaction 2,
`updateHeadsetPoseField`, accepted an injected pose with `valid: true` (`notes/23`). Step 4 is no
longer a research question.

## Immediate next steps (all unattended)

1. **Injection daemon (step 4, critical path).** Drive `updateHeadsetPoseField` at frame rate from a
   process instead of one-shot `service call`; feed it our VIO. Then the criterion that actually
   matters: **does Meta's compositor render from it?** A pose in `dumpsys` is not proof.
   Open sub-problem: injection **zeroes velocities**, and fields 2/3 accept 3-vectors that do not
   appear in `pos_vel`/`rot_vel`. Without velocity, predicted poses will lag.
2. **Pose logger ≥ 30 Hz (step 2).** `trackinginterface_cli` is **3.3 Hz** and imports zero
   `libossdk` symbols (statically linked), so there is no API to borrow. Route:
   `ITrackingService::getSharedMemoryFileDescriptor`, or read a client's existing mapping
   (`/dev/ashmem/TrackingServiceController`, 16 KB, in `com.oculus.vrguardianservice`).
   Needs the panel **on** and the sensor covered to have live data to search for.
3. **Controller decode (step 3).** `tools/sb_survey/` catalogues MCU packet types. Types so far:
   `0x50` IMU @998.8 Hz, `0xe0` exposure @31.8 Hz, `0x46` once. Controller packets have not been
   seen yet — they likely need their own enable, as the IMU did (type 110).

Then **one worn session** serves steps 2 and 3 together: ≥ 2 min worn with walking and fast
rotation, plus ~50 discrete button/trigger/thumbstick events.

## Device state and traps

- Running **our instrumented kernel** (`4.4.205-perf+ #2`), Magisk root intact. Backups in
  `backups/` including `persist-backup.tar` (14 MB, all factory serials/calibration).
- **Panel is blanked.** Restore: `adb shell su -c 'echo 0 > /sys/class/graphics/fb0/blank'`.
  Blanking **stops head tracking**; panel must be on for pose work.
- **UFS link dies on idle clock-gating.** After that, `adb push` reports success while writing only
  page cache — silently invalidating captures. Scripts call `ufs_pin`. Recover with
  `echo 1 > /proc/sys/kernel/sysrq; echo b > /proc/sysrq-trigger` (`adb reboot` blocks on unmount).
- **Proximity cover is required** for any 6DOF work and cannot be faked in software:
  `sys.hmt.mounted` is an *output*. `require_covered` (in `tools/device/devctl.sh`) must run
  **before** services are stopped.
- Capture: `cam_kernel <ncam> <secs> <exp> <gain> <savecams> <outdir>`, e.g.
  `4 75 3000 160 02 /data/local/tmp/vio-b2`, then `build_euroc_direct.py`.
  **IMU needs its own MCU enable (type 110)** — a camera probe alone yields zero 0x50 packets.
- OpenVINS runs under podman; volumes need `:z` and cannot live in `/tmp` (SELinux). Use
  `work/vio-run/`, and `LD_LIBRARY_PATH=/usr/local/lib`.

## Method rules earned the hard way

1. **Diff the failing path against the working control, through the identical measurement.** Five
   sessions instrumenting B2 alone found nothing; one run each of B2 and B1 with the same
   `dynamic_debug` files found it immediately. Identical config everywhere *plus zero interrupts
   anywhere* means starved, not misconfigured.
2. **A threshold is meaningless until the reference is measured against it.** Two step-1 criteria
   were nearly reported as failures; B1 scores 6.4–12.0 LSB against its *own* frames on a bar of 2.
3. **Beware bugs masked by a reduced test case.** `ncam=1` hid both B2 bugs for five sessions.
4. **One change at a time**, and validate the harness with a no-op change first (stock kernel +
   `LEGACYSAR` reproduced the known-good image byte-identically before anything new was flashed).
