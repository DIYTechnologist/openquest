# Step 4 task 1 — pose injection works — 2026-09-04

`notes/18` step 4 task 1: *"Probe `TrackingDataInjection` — if it accepts external poses, it is a
large shortcut and needs no RE of the producer side."*

**It accepts external poses.** Meta's tracker reported our injected pose with `valid: true`.

```
before : pos=( -0.2196, -0.2322, -1.3729)  quat=(0.906, 0.027, 0.422, 0.011) valid=True
inject : updateHeadsetPoseField(0, {5,6,7})  ->  Parcel(00000000 00000001)   = true
after  : pos=(  5.0000,  6.0000,  7.0000)  quat=(1.000, 0.000, 0.000, 0.000) valid=True
```

This removes the largest unknown in step 4. We do **not** need to reverse-engineer
`ITrackingService`'s producer side or register an adapter under Meta's name — there is a supported,
named entry point for exactly this.

## Interface

`TrackingDataInjection` → `oculus.internal.virtual_input.ITrackingDataInjectionService`, registered
and live. Transaction codes recovered by disassembling the `Bp` proxies in
`/system/lib64/libossdk.oculus.so` (the `mov w1, #N` immediately before the `transact` vcall):

| code | method |
|---|---|
| 1 | `updateRemotePoseField(String16 id, int field, vector<float>, bool*)` |
| 2 | `updateHeadsetPoseField(int field, vector<float>, bool*)` |
| 3 | `updateRemoteButtons(String16 id, vector<ButtonState>, bool*)` |
| 4 | `moveRemoteThumbstick(String16 id, int, int, bool*)` |
| 5 | `setTrackingMode(int, bool*)` |

Parcel layout for code 2, read off the disassembly: `writeInterfaceToken`, `writeInt32(field)`,
`writeFloatVector(values)`. Reply is a `binder::Status` followed by the `bool`.

### Field map

Established from return status (1 = accepted, 0 = rejected on wrong arity) plus observed effect:

| field | arity | meaning |
|---|---|---|
| 0 | 3 | position (x, y, z) — **verified end to end** |
| 1 | 4 | orientation quaternion, **(x, y, z, w) order** — **verified** |
| 2 | 3 | accepted; not reflected in `pos_vel_*` |
| 3 | 3 | accepted; not reflected in `rot_vel_*` |

Quaternion order is not the CLI's display order: sending `{0.1,0.2,0.3,0.4}` reads back as
`w=0.4, x=0.1, y=0.2, z=0.3`, so the **wire order is xyzw** while `getHeadTrackingData` prints wxyz.
Getting this backwards would produce a plausible-looking but wrong orientation, so it is worth
stating explicitly.

Injecting position **zeroes the velocities** (`pos_vel`, `rot_vel` all 0.0). Fields 2 and 3 accept
3-vectors but their values do not appear in the velocity fields; identifying them matters for
prediction quality (Meta's compositor predicts ahead), so it is an open item, not a solved one.

## Access control

- **root: accepted** — `Parcel(00000000 00000001)`
- **shell: rejected** — `Parcel(ffffffff 00000000 ...)`

So this needs root, which we have. Note `service call` intermittently reported "Service
TrackingDataInjection does not exist" while `service check` found it in the same second; retrying
worked. Transient lookup failure, not a permissions issue — worth knowing before chasing it.

## Reversibility

Injected state persists until `trackingservice` restarts:
```sh
stop trackingservice; sleep 3; start trackingservice
```
Verified: pose returned to `(-0.2206, -0.2341, -1.3755)`, matching the pre-injection reading, at
6DOF `Valid: Yes`.

## What this changes

Step 4's acceptance criteria become directly reachable — `dumpsys tracking` reporting our poses is
already demonstrated in miniature. Remaining work is no longer *"can we?"* but engineering:

1. Drive injection at frame rate from a daemon rather than one-shot `service call`.
2. Feed it our VIO output instead of constants.
3. Confirm Meta's shell and compositor actually consume it (a pose in `dumpsys` is not proof the
   compositor renders from it).
4. Identify fields 2/3 so velocity is populated — with zeroed velocity, predicted poses will lag.

**Caveat worth keeping honest:** this proves the *interface* accepts and republishes a pose. It does
not yet prove the compositor renders from that pose, which is the criterion that actually matters.

---

# Injection daemon — frame rate achieved, real VIO replayed — 2026-09-04

`tools/pose_inject/` drives the interface from one process instead of one-shot `service call`.

**Why a daemon was needed:** `service call` costs ~21 ms per invocation (process spawn), and a pose
needs **two** transactions — position and orientation are separate fields — so it tops out near
23 Hz, under frame rate.

## How it binds, without any Meta library

Uses the **stable NDK binder C API** (`libbinder_ndk`, Android 10+), not `libbinder`'s C++ ABI, so
nothing depends on a Meta blob or on matching a C++ ABI we do not control:

```
AServiceManager_getService("TrackingDataInjection")
AIBinder_Class_define("oculus.internal.virtual_input.ITrackingDataInjectionService", ...)
AIBinder_associateClass()          // so prepareTransaction writes the right interface token
AIBinder_prepareTransaction() -> AParcel_writeInt32(field) -> AParcel_writeFloatArray(v, n)
AIBinder_transact(binder, 2, ...)
```

One wrinkle: `AServiceManager_getService` lives in `<android/binder_manager.h>`, which the NDK does
not ship, **and the NDK's stub `libbinder_ndk` does not export it either** — so it cannot be linked
and is resolved with `dlopen`/`dlsym` against the device library. Everything else links normally.

## Measured

| target | achieved | failed | missed deadline | worst overrun |
|---|---|---|---|---|
| 30 Hz | 30.0 Hz | 0 | **0 (0.0 %)** | 0.0 ms |
| 60 Hz | 59.9 Hz | 0 | 1 (0.3 %) | 12.9 ms |
| 120 Hz | 119.5 Hz | 0 | 5 (0.7 %) | 9.0 ms |
| 240 Hz | 237.3 Hz | 0 | 21 (1.5 %) | 18.0 ms |

**8× headroom over frame rate, at 6.4 % of one core** — it coexists with VIO's 31.75 ms/frame.

## Correctness

Synthetic circle (r = 0.5, yaw-coupled) read back through `getHeadTrackingData`: radius **exactly
0.500** on every sample, `y` exactly 0.000, `valid: true` throughout.

Then the real thing — **our own VIO trajectory** from the open camera stack (`notes/22`), 621 poses
replayed at 30 Hz:

```
our VIO final : -0.026141581  0.092579816  0.040967583 | 0.543203203 -0.413153257 -0.459988539 0.568018671
Meta reports  : -0.026141580 +0.092579819 +0.040967584 | +0.543203175 -0.413153261 -0.459988534 +0.568018675
621 poses @ 30.0 Hz, 0 failed, 0 missed deadlines
```

Agreement to ~1e-8 — float32 round-trip precision. **The full chain runs: open cameras → open VIO →
Meta's tracker**, with no Meta userspace code anywhere in it.

## Field map closed out

Fields **4 and 5 are rejected** (return `false`), so the valid range is **0–3**:

| field | arity | status |
|---|---|---|
| 0 | 3 | position — works, verified to float precision |
| 1 | 4 | orientation quat (xyzw) — works, verified |
| 2 | 3 | accepted (`true`) but **never observable** |
| 3 | 3 | accepted (`true`) but **never observable** |

Fields 2/3 were injected with `{7,8,9}` and checked against `pos_vel`, `pos_accel`, `rot_vel` and
`rot_accel` — **all remain 0.000**. So velocity/acceleration cannot currently be driven, and stays
zeroed under injection. Either they write state `getHeadTrackingData` does not expose, or they are
consumed elsewhere. Recorded as unresolved rather than guessed.

**Why it matters:** Meta's compositor predicts ahead using velocity. With velocity pinned at zero,
prediction degenerates to "pose holds still", which should show up as motion-to-photon latency
rather than breakage. This is the most likely cause if the rendered result feels laggy.

## What is still NOT proven

A pose in `dumpsys`/`getHeadTrackingData` is **not** proof the compositor renders from it. The
step-4 criterion — *Meta's shell responds to head motion for ≥ 10 minutes* — needs someone looking
through the headset while injection runs. That is the next real test, and it needs the wearer.

---

# Confirmed: the compositor renders from the injected pose — 2026-09-04

The caveat carried through this whole note — *"a pose in `dumpsys` is not proof the compositor
renders from it"* — is now resolved. **It does.**

**Test.** Headset worn (so the proximity sensor is genuinely covered and tracking is 6DOF).
10 s baseline with no injection to establish that the view follows the head normally, then 25 s of
`spin.txt`: a continuous 360° yaw at 14.4°/s, injected at 30 Hz. Continuous and non-reversing by
design, so "did the world move or did I move?" has no ambiguous answer.

**Result.** The wearer reported the world drifting steadily sideways for the duration. 750 poses,
30.0 Hz, 0 failed, 0 missed deadlines, no exceptions.

Two earlier attempts were inconclusive because of test design, not the mechanism: ±15° at 0.1 Hz was
too subtle to distinguish from ordinary head motion, and instructions cannot be read while the
headset is on. Fixed by briefing before donning, adding a no-injection baseline, and using a
continuous one-directional motion.

**What this closes.** Step 4's first acceptance criterion (`dumpsys tracking` reporting our poses at
6DOF, `Valid: Yes`) is met, and the load-bearing assumption behind it is verified rather than
assumed. Meta's compositor is a usable consumer of an external pose source.

## The next obstacle is device contention, not the interface

Everything so far replays a **recorded** trajectory. Closing the loop live — camera → VIO →
injection in real time — runs into a conflict already flagged in `notes/18` step 2:

- `cam_kernel` needs `trackingservice` **stopped** (`/dev/video0` and `/dev/syncboss0` are
  single-open, and `trackingservice` holds them).
- Injection needs `trackingservice` **running** — it is the service being injected into.

So on the stock OS, our camera stack and our injection target cannot run simultaneously by that
route. This does not affect the OS swap (step 5), where we own the whole stack and nothing competes;
it affects only the in-place demo. Options, unverified:

1. Revive the **leech** (`notes/09`/`notes/10`) to read frames while `trackingservice` runs — the
   same tool step 2 needs for ground truth, so it serves twice.
2. Establish exactly which nodes actually conflict, rather than assuming all of them do.

Recorded as the next thing to determine. It does **not** invalidate what is proven here: the
injection path works, at frame rate, with our own poses, and reaches the display.
