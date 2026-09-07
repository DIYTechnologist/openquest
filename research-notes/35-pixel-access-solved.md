# Pixel access at FrameSet rate: solved — 2026-09-05

The blocker behind step 2 since `notes/31` is resolved. Pixels can now be read at the **145 Hz
FrameSet rate** instead of only during allocation bursts. One refinement remains before frames can
be dumped (below), but the mechanism works and `trackingservice` is unharmed.

## Why every VA-tracking approach was doomed

`notes/31` and `notes/33` both tried to track `id -> pixel VA` from the ctor and read that VA later.
Measured, every probe of a tracked VA failed — via `process_vm_readv` **and** via
`/proc/self/mem`, including probes taken while ctor activity was live:

```
4000 FrameSet records, 16 slots probed each: non-zero results = 0
```

The VA is not stale-sometimes; it is simply **gone**. `notes/10` recorded the reason and it was read
past for four notes: `this+0x60` is the *locked* CPU pixel VA. Meta locks the gralloc buffer for the
ctor and unlocks it after, so between bursts there is nothing mapped. **No VA-tracking scheme can
ever work**, and the several attempts to refine one were chasing a dead design.

## What works: take the ctor's arguments, not its result

The ctor is `ImageBuffer(const native_handle* meta, const native_handle* pixels)` — the handles are
in x1/x2. `tools/cam_tap/ibfs_hook5.c` widens the trampoline to preserve them, then:

1. reads the pixel `native_handle` (`{version, numFds, numInts, data[]}`; measured **numFds = 2**),
2. `dup`s `data[0]` — the dmabuf fd — because Meta owns and will close the original,
3. `mmap`s it **itself**, keeping a per-id table, re-mapping only when the handle changes.

Our mapping is independent of Meta's lock/unlock cycle, so it stays valid indefinitely. Result:

```
IB ... id=0 640x481 nfds=2 mapped=1 ourva=782e399000
FS ... buf=14 c0=0:... c1=1:... c2=2:... c3=3:...  H: 0:0c35cecf 1:3007ab5c 2:4569c71e ...
```

All 16 slots return live, distinct pixel hashes at FrameSet rate. `trackingservice` survived every
run.

**One trap worth recording:** reads of our own mapping via `/proc/self/mem` still returned `EIO`. A
dmabuf mapping is `VM_PFNMAP`, which `access_remote_vm` refuses, so pread fails on a perfectly valid
mapping. Direct dereference is both correct and safe here — precisely because the mapping is ours
and pinned by a dup'd fd, unlike Meta's transient VA.

## The id <-> FrameSet index mapping is a learned bijection

Correlating which slot's pixels change against the FrameSet index, over 3999 transitions:

```
slot  0 -> buf  0     slot  4 -> buf  7     slot  8 -> buf  5     slot 12 -> buf  4
slot  1 -> buf 14     slot  5 -> buf  1     slot  9 -> buf 13     slot 13 -> buf 11
slot  2 -> buf 10     slot  6 -> buf 12     slot 10 -> buf 15     slot 14 -> buf  8
slot  3 -> buf  9     slot  7 -> buf  2     slot 11 -> buf  3     slot 15 -> buf  6
```

Each slot changes ~86 times and is associated with a single index at ~97 % consistency. It is a
**permutation, not a formula** — it depends on allocation order, so it must be learned at runtime
rather than hard-coded. The hook already learns it for free.

## The remaining refinement: four cameras share one id

```
576 ctor calls / 16 ids = 36 per id = 4 cameras x 9 rounds
```

`id` is the **FrameSet ring index**, not a per-image handle: all four cameras of a frameset carry the
same `id`, which matches the descriptor, where all four blocks show the same index and differ only
in `camId` (`notes/34`). The per-id table therefore keeps whichever of the four ctors happened last,
so we currently capture **one arbitrary camera per frameset** rather than a chosen stereo pair.

This also retires `notes/10`'s `cam = id & 3`. That held only inside allocation bursts, where
consecutive ctors happen to be consecutive cameras; it is not a property of `id`.

**Fix:** key the table on `(id, k)` where `k` is the position of the ctor within the run of four
consecutive same-id calls, then confirm `k -> camId` against the descriptor's per-camera timestamps,
which are already logged. Cheap, and fully desk-testable thanks to the proximity bypass
(`notes/34`).

## Status

| | |
|---|---|
| pixels readable at FrameSet rate | **yes** |
| survives Meta's lock/unlock | **yes** — our own dmabuf mapping |
| id <-> FrameSet index | **solved**, learned at runtime |
| which of 4 cameras a mapping holds | **open** — the one refinement left |
| full-rate stereo dump | not yet — needs the above |

Device restored: `prox_open`, `trackingservice` clean, SELinux Enforcing.
