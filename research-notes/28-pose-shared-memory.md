# Step 2 — Meta's head-pose shared memory located — 2026-09-04

`notes/18` step 2 needs Meta's poses logged at **>= 30 Hz**. `trackinginterface_cli` manages **3.3 Hz**
(process spawn + Binder per call) and imports **zero** symbols from `libossdk.oculus.so` — the
tracking client is statically linked, so there is no library API to borrow. The poses had to be read
from shared memory directly.

## Method: use injection as a known-value generator

The obstacle was needing live, *known* pose values to search for, which normally means a wearer
(tracking is gated to 0DOF off-head). It does not: **injection works with the proximity sensor
uncovered**, so an arbitrary distinctive pose can be planted and then searched for.

```
inject pos = (12345.678, -98765.4, 4242.42)  ->  readback pos_x = 12345.677734375, valid = true
search for  b6e64046 b3e6c0c7 5c938445
```

## Result

```
7f1f1c8000-7f1f1ca000 rw-s  /dev/ashmem/TrackingServiceHeadTracker (deleted)     8 KB
```

Hits land at 0x038, 0x0d8, 0x67c, 0x71c, 0xa3c — a **ring of pose slots, 160-byte stride**. Record
layout, read off the surrounding bytes:

```
0030  00000000 00000000 0000803f b6e64046
0040  b3e6c0c7 5c938445 ...
        ^quat x,y,z,w (0,0,0,1)  ^pos x,y,z
```

**Quaternion (x, y, z, w) immediately followed by position (x, y, z), all float32** — the same
xyzw wire order as the injection interface (`notes/23`), not the wxyz order the CLI prints.
Velocity/acceleration fields follow and were zero, consistent with injection zeroing them.

A logger can therefore `pread` this region out of `trackingservice` via `/proc/<pid>/mem`, or map it
properly through `ITrackingService::getSharedMemoryFileDescriptor`, and sample at any rate. Remaining
work: identify the sequence/timestamp field that says which slot is current, so the newest pose can
be picked without scanning.

## Two bugs in our own `mempeek`, both silent

Neither produced an error; both produced confident, wrong, negative results.

1. **The region filter skipped everything.** `if (g_regs[i].perms[1] != 'r') continue;` — `perms` is
   `"rw-p"`-style, so index 0 is the read bit and index 1 is *write*. The test was true for every
   region, so `scan` never examined a single byte. **Every "0 hits" this tool ever reported is
   meaningless.**
2. **The hex pattern parser only read one byte.** `strtoul(h, &end, 16)` sets `end` into the *local*
   buffer `h`, and the code then did `p = end`, walking the cursor into stack memory instead of along
   the input. Patterns were effectively 1 byte, so scans "matched" constantly and uselessly.

The first bug masked the second: with no regions scanned, a broken pattern parser produced no
symptoms. Both fixed; the search then found the region on the first attempt.

**Method note:** three separate times this session a tool reported a clean negative that was actually
a defect in the tool — `dumpsys | grep -m1` killing `trackingservice`, and both of these. A negative
result from an instrument that has never produced a positive is not evidence. The fix is cheap:
before trusting "not found", confirm the tool can find something known to be there.

---

# `tools/pose_log/` — logger built and acceptance criterion met

Reads the region out of `trackingservice` via `/proc/<pid>/mem`, resolving the address by **name**
from `/proc/<pid>/maps` each run (it is not stable across restarts). A read-only `pread` was chosen
over mapping the ashmem fd through `ITrackingService::getSharedMemoryFileDescriptor`: for a
measurement tool it is simpler and cannot perturb the producer.

Per sample: read `seq`, compute `slot = seq % nslots`, read that slot's quaternion and position.

## Validation against a known signal

Rather than trust it on live data, it was checked against an **injected circle of radius 0.5** —
a signal whose correct answer is known exactly:

```
100 Hz sampling, 12 s:   radius mean = 0.5000  min = 0.5000  max = 0.5000, 0 read errors
```

Exact to four decimals at every sample, so the slot selection is not tearing.

## Acceptance criterion (notes/18 step 2)

*"Meta poses logged at >= 30 Hz for >= 120 s with < 1 % dropped samples"*:

| metric | result |
|---|---|
| duration | **125.0 s** |
| rate | **59.97 Hz** |
| interval | median 16.67 ms, p99 17.25 ms, max 98.95 ms |
| late (> 1.5x period) | **5 / 7494 = 0.067 %** |
| read errors | **0** |
| radius over the whole run | 0.5000 / 0.5000 / 0.5000 |

**Criterion met**, with ~2x the required rate.

One caveat on interpretation: the *distinct pose* rate here is 34 Hz, but that is bounded by the
**source** — the injection driving this test publishes at 30 Hz — not by the logger, which sampled
at 60 Hz without error. Against live tracking the distinct rate will be whatever Meta produces.

## Simultaneity: tested, and it does NOT work

The ground-truth comparison needs our sensors and Meta's poses in the **same session**. Measured:

| configuration | result |
|---|---|
| sensors HAL **up** | logger works; **30 Hz of genuine Meta poses**, no injection needed |
| sensors HAL **down** + our camera capture | camera capture fine (4/4 pipelines, 481 frames/cam, 0 Meta libs) but **`trackingservice` restarts and no longer maps `TrackingServiceHeadTracker`** — the logger reports "not mapped in pid 29682" |
| HAL restarted | region reappears, logger back to 30 Hz |

So stopping the HAL does not merely inconvenience Meta's tracker, it removes its pose output
entirely — which makes sense, since with no sensor source there is nothing to track with.

**This sharpens the earlier correction in `notes/23`, it does not undo it.** Two different questions:

- **Camera capture + pose *injection*** — **works.** That is what step 4's live closed loop needs,
  and it is proven.
- **Camera capture + Meta's own *tracking output*** — **does not work.** That is what step 2's
  ground truth needs.

So step 2 is back to the route `notes/18` originally specified: **revive the leech** (`notes/09`,
`notes/10`) to read frames while the HAL and `trackingservice` both run. The non-simultaneous
fallback (walk the same route twice) remains available but is much weaker.

Incidental confirmation: the region address moved from `0x7f1f1c8000` to `0x7034453000` across a
restart, so resolving it by name from `/proc/<pid>/maps` rather than hardcoding was necessary.
