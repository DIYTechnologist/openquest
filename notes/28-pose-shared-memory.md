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
