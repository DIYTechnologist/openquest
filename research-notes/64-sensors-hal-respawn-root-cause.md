# Sensors-HAL respawn: root cause found — it's Android's own SensorService, not a VR watchdog — 2026-09-09

`research-notes/55` (2026-09-07) found `stop vendor.oculus.sensors-hal-1-0` no longer stays stopped
and left the cause undiagnosed. Needed to know why before attempting the live 10-minute closed-loop
soak this session set out to do (step 4's last non-hardware-blocked criterion) — `cam_kernel` needs
that HAL down for exclusive `/dev/video0` access. Diagnosed directly rather than guessing forward.

## Reproduced, then isolated

First reproduced with the *correct* service name (`vendor.oculus.sensors-hal-1-0` — the earlier
attempt this session used the process name, `vendor.oculus.hardware.sensors@1.0-service`, by
mistake, which is a no-op `stop` target and looked like "nothing happens" rather than "respawns").
With the right name: `stop` genuinely kills it, and it is back with a new PID within ~1-2s, matching
`research-notes/55` exactly.

Ruled out every VR-specific process as the trigger, progressively:

| stopped alongside the HAL | HAL still respawns? |
|---|---|
| (nothing else) | yes |
| `trackingservice` | yes |
| `trackingservice` + `calibration_svr` + `mrsystemservice` + `sensorproxy` + `cameramuxmodeservice` | yes |

Every process this service's own `.rc` lists under `onrestart` (`calibration_svr`, `mrsystemservice`,
`sensorproxy`, `trackingservice`) was stopped simultaneously and it made no difference.

## The actual cause, caught directly in logcat

```
SensorService: Sensors HAL died, attempting to reconnect.
ServiceManagement: getService: Trying again for android.hardware.sensors@2.0::ISensors/default...
[...]
SensorService: Oculus SyncBoss hal version: v2.28.0. [...]
MontereyCameraProvider: Starting MontereyCameraProvider SensorService (4 cameras)
ServiceManagement: Registered vendor.oculus.hardware.sensors@1.0::ICameraProvider/default
ServiceManagement: Registered android.hardware.sensors@2.0::ISensors/default
```

`/vendor/etc/init/vendor.oculus.hardware.sensors@1.0-service.rc` registers **five HIDL interfaces
from one binary**: `ICameraProvider`, `IControllerProvider`, `IImu`, `IMag`, and — critically —
`android.hardware.sensors@2.0::ISensors`, the **standard Android system sensor HAL interface**.
`android`'s own core `SensorService` (part of `system_server`, pid 1058 — not anything Oculus-
specific, not stoppable without stopping the whole framework) holds a permanent client connection to
`ISensors` for ordinary Android sensor plumbing (it's also how the "Syncboss Double Tap Virtual
Sensor" seen in `dumpsys sensorservice` gets served). The moment the HAL process dies,
`SensorService` notices the binder death, logs exactly that, and immediately calls `getService()`
again — which is what triggers `hwservicemanager`'s lazy-HAL auto-start, respawning the **entire**
bundled binary, camera provider included, regardless of which VR processes are up or down.

## What this means

This is core Android/Treble architecture, not a fixable VR-side watchdog. There is no clean way to
keep this HAL down for any sustained duration while `system_server` is up — and `system_server`
being up is not optional, since stopping it would also take `trackingservice` (needed for injection)
with it. `pkill -9` on the HAL (the option this session avoided pending user confirmation) would not
have helped either: the trigger is a legitimate, correctly-functioning reconnect in core Android
code, not a hung process a `-9` would clear.

**This also reconciles with `research-notes/23`'s "corrected" claim** that stopping only the sensors
HAL was sufficient (verified 2026-09-04, a `cam_kernel` run completed cleanly). That result wasn't
wrong, it was under-timed: a short capture can complete inside the reconnect race's slack before
`SensorService` notices and respawns. A **sustained** 10-minute session cannot — the HAL will be back
within 1-3 seconds and start fighting `cam_kernel` for the device.

## What's actually viable for the live 10-minute soak

The sensors HAL must **never be stopped** for any approach that needs to survive minutes, not
seconds. The existing `tools/cam_tap/ibfs_hook9.so` + `tools/capture/ts_ibfs9.sh` leech technique
(built for the controller-tracker captures, `research-notes/56`) already does exactly this — it
relaunches `trackingservice` itself with the hook `LD_PRELOAD`ed, tapping frames from *inside*
Meta's own already-running, already-working camera pipeline, and never touches the sensors HAL at
all. It currently only dumps to a file for offline analysis. Adapting it to stream tapped frames live
into `vio_live` (instead of, or alongside, a file) would sidestep this whole respawn problem entirely
for the live soak, reusing proven infrastructure rather than fighting core Android HAL lifecycle
behaviour. Not yet attempted — real follow-on engineering work, not a quick fix.

## Housekeeping

All services touched during diagnosis (`calibration_svr`, `mrsystemservice`, `trackingservice`,
`cameramuxmodeservice`, the sensors HAL) were back to their normal running state via the respawn
cascade or an explicit `start`; verified via `getprop init.svc.*` before finishing. No capture data
involved — this was pure service/logcat diagnosis.
