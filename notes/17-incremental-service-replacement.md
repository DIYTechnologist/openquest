# Incremental service replacement, before any OS swap — 2026-09-03

Strategy proposed by the user: replace Meta's services **one at a time on the existing OS**, rather
than doing a big-bang OS replacement. This note records why that is viable, the order, and what it
buys — plus where it is throwaway work.

## Why it works: every Meta service is a named Binder interface

`service list` on the stock OS:

```
tracking              oculus.internal.tracking.ITrackingService
TrackingEnvironment   oculus.internal.ITrackingEnvironment
TrackingDataInjection oculus.internal.virtual_input.ITrackingDataInjectionService
TrackingFidelityService oculus.internal.ITrackingFidelityService
TrackedObjectService  oculus.internal.trackedobject.ITrackedObjectService
HandTrackingService   oculus.internal.IHandTrackingService
VrApi                 oculus.internal.IVrApiService
HMDCalibration        oculus.internal.ICalibrationService
cameramuxmodeservice  oculus.internal.ICameraMuxModeService
vrfocus               oculus.internal.IVrFocusService
```

Named interfaces mean a replacement can register under the same name and Meta's existing clients
bind to it transparently. That is the whole basis for incremental substitution: **the seams are
already there.**

Note `TrackingDataInjection` — an interface whose evident purpose is feeding tracking data *in*
(Meta presumably uses it for replay/synthetic testing). If it does what the name suggests, it is a
supported-ish route to driving Meta's own shell and compositor from **our** poses, on the stock OS,
with no reverse engineering of the pose *producer* side at all. Unverified; worth an early look
because it would be a large shortcut.

## The bigger prize: this solves the ground-truth problem

`notes/14` closed the VIO validation as far as it could go offline and stopped at "no absolute
ground truth". But Meta ships a production-grade tracker on this exact hardware, and its output is
readable:

```
trackinginterface_cli getHeadTrackingData [prediction_ms] [json]   # from shared memory
trackinginterface_cli getcontrollertrackingdata [prediction_ms] [json]
trackinginterface_cli getcontrollerbuttondata [json]
dumpsys tracking      # pose, linear/angular vel+accel, ReferenceFromOdometry, tracking level
```

**Meta's tracker is the reference trajectory we lacked.** Same rig, same sensors, no mocap needed.

**The catch, and it is a real one.** `cam_direct` requires `trackingservice` stopped (`/dev/video0`
is single-open), so we cannot log Meta's poses and capture our own raw sensor data *simultaneously*
by that route. Options, in rough order of appeal:
1. The older in-process camera **leech** (`notes/09`, `notes/10`) read trackingservice's shared
   dmabufs *while it ran*. It was abandoned because frame↔timestamp linking was lossy — but for a
   ground-truth comparison a fraction of frames may be enough, and the "ATOMIC hook" idea in
   `notes/08` was the fix for exactly that.
2. Accept non-simultaneous comparison (same route walked twice) — much weaker, but nearly free.

Verified only that the CLI exists and that `dumpsys tracking` prints poses. Live output was empty
here because the headset was on a desk in STANDBY/0DOF — the proximity sensor gates tracking, so
confirming the JSON format needs the headset worn.

## Order, and the reasoning

| # | Service | Why here |
|---|---|---|
| 1 | `HMDCalibration` / `calibration_svr` | We already bypass it; cheapest possible rehearsal of registering a replacement Binder service and having Meta's clients accept it |
| 2 | **`trackingservice`** | Highest value. We already have the replacement algorithm (`notes/14`). Forces real-time on-device execution, and `TrackingDataInjection` may make it cheap |
| 3 | `cameramuxmodeservice` | Effectively bypassed already by `cam_direct` |
| 4 | Sensors HAL (`vendor.oculus.hardware.sensors@1.0`: `IImu`, `ICameraProvider`, **`IControllerProvider`**) | `IControllerProvider` is a direct lead on the controller problem, which `notes/01` rates our lowest-confidence area |
| 5 | `vrapi_svr` / `VrApi` | Last. Hardest, and the most throwaway — see below |

## Where this is throwaway work — be deliberate

Conforming to Meta's closed `oculus.internal.*` AIDL interfaces produces shims that get **deleted**
when the stack moves to Monado, which has its own interfaces. So pick targets where the payoff is
validation or knowledge rather than permanent code:

- **Worth it:** `trackingservice` (proves our VIO real-time on-device, and unlocks Meta as ground
  truth), sensors HAL `IControllerProvider` (buys down the biggest unknown).
- **Not worth it:** in-place replacement of `vrapi_svr`. That is the compositor; building it against
  Meta's interface is pure throwaway when the target is Monado's compositor. Skip straight to
  Monado for that layer.

## Two honest caveats

1. **This does not serve the security motivation.** Two stated motivations for the project are
   e-waste and closing CVEs; the device is stuck on Android 10 / kernel 4.4.205 (EOL Feb 2022).
   Incremental service replacement leaves that base entirely intact. It de-risks the *engineering*,
   it does not deliver the *security* goal — only the OS swap does.
2. **`devicecert` attestation.** `vendor.oculus.hardware.devicecert@1.0` is flagged in `notes/01` as
   something to understand *before* removal. Unknown what it gates; check before disabling services
   that might depend on it. SELinux policy will also need handling per service (we run permissive
   during experiments today, which is not a shippable answer).
