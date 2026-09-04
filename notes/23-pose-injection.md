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
