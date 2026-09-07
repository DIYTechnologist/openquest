# Pool mapping: `id` is not a buffer handle, and the fix cannot be desk-validated — 2026-09-05

Diagnostic pass on `notes/31`'s proposed frame-capture fix ("learn `slot -> pixel VA` at ctor time,
read pixels on each FrameSet read"). Two corrections to `notes/31`, one supporting result.

## Correction 1: `id` does not identify a stable buffer

`notes/31` treated the `ImageBuffer` `id` (0..15) as a **pool slot**, implying a stable
`slot -> pixel VA` mapping. Measured with `tools/cam_tap/ibfs_hook3.c` on a desk startup burst:

```
448 ctor calls, ids 0..15, exactly 28 calls per id
id -> distinct pixel VAs:  24 for every id
distinct pixel VAs overall: 384      distinct `this`: 427
```

Every id appears with **24 different pixel VAs**. In the worn session the same measure is worse —
48 ctor calls per id spanning ~40 distinct VAs. So `id` is a rolling index, not a handle, and there
is no single VA to learn per id.

This means `notes/31` was wrong to "correct" `notes/10` on this point. `notes/10`'s original claim —
"sd/pixel VAs move every frame -> a persistent-VA poller would fail" — is **right about the VAs**.
Both notes were partly right and I conflated two separate facts:

- **the ctor is not per-frame** (`notes/31`, correct — 768 calls in 4 bursts over 151 s while frames
  were delivered at 119 Hz);
- **the VAs are not stable** (`notes/10`, correct — they churn on every ctor call).

The fix is not dead, but it is narrower than stated: the usable quantity is the **most recent ctor
per id**, which is well defined *between* bursts, where the 119 Hz of FrameSet reads actually
happen. Whether that VA still holds the live pixels between bursts is unverified.

## Correction 2: it cannot be validated on a desk

`notes/31` claimed the fix "is testable on a desk, because the startup burst fills the pool". Half
true, and the wrong half is the important one:

```
desk run: IB ctor lines 448   FS reads 0
worn run: IB ctor lines 768   FS reads 12000 (119 Hz)
```

`MessageQueue<FrameSet>::read()` **does not fire at all with the headset off a head** — tracking is
gated by the proximity sensor. The ctor side is desk-testable; the FrameSet-triggered read, which is
the whole mechanism, is not. So the integration can be *built* unattended but only *validated*
worn. Plan accordingly rather than discovering it mid-session.

## Supporting result: the FrameSet block fields are pinned down

Full 52-word descriptors were not obtainable (they need active tracking), but the worn session's
12,000 16-word records settle the two fields we had:

```
w2  : hi = 0..15 (buffer index)   lo = 0  ALWAYS   -> camId 0
w14 : hi = 0..15 (buffer index)   lo = 1  ALWAYS   -> camId 1
```

`lo` is constant across all 12,000 records, so it is the **camera id**, not data — confirming the
block layout (`notes/10`: 4 image blocks on a 12-word stride, heads at w2/w14/w26/w38). `notes/31`
read `w2`/`w14` both showing buffer 5 as suspicious; it is not — the two blocks legitimately
reference the same buffer index for two different cameras of a stereo pair.

Cross-check over the whole session: every FrameSet buffer-index reference had a prior ctor for that
id — 24,000 of 24,000, **100 %**. That is necessary but not sufficient (with only 16 ids and 768
ctor calls, prior coverage is nearly automatic), so it does not by itself validate the mapping.

## Where this leaves the frame fix

| question | state |
|---|---|
| ctor fires per frame? | **no** — pool allocation events only |
| `id -> pixel VA` stable? | **no** — ~24-40 distinct VAs per id |
| most-recent-VA-per-id usable between bursts? | **plausible, unverified** |
| FrameSet names buffer index + camId? | **yes** — `hi` = index, `lo` = camId |
| desk-validatable? | **no** — FrameSet reads need active tracking |

Better alternative worth costing before building the VA approach: the FrameSet carries the two
`native_handle`s the ImageBuffer is constructed *from* (`notes/09`), and `tools/dmabuf_read/`
already exists. Mapping the dmabuf directly sidesteps VA tracking entirely and does not care how
often the pool churns. It needs the full 52-word descriptor to locate the handles — which needs a
worn session to capture.
