# Camera identification: two groups of eight, and why the last step needs motion — 2026-09-05

Follow-on to `notes/35`, which left one question: which of the 16 pooled buffers holds which camera.
Answered as far as a static scene allows, and the limit is now understood rather than guessed at.

## Ground truth instead of a fourth inference

Three inferences about this have been wrong (`notes/10`'s `cam = id & 3`, `notes/31`'s slot theory,
and `notes/35`'s slot<->index bijection, which cannot be the whole story since it assigns one slot
per frameset index while a frameset carries four images). So `tools/cam_tap/ibfs_hook6.c` dumps a
full 640x481 frame from **every** slot at a single instant and looks at the pixels.

That works: 16 x 307,840 B written from our own dmabuf mappings, `trackingservice` unaffected.

## Result: a clean 2 x 8 split, not 4 x 4

Pairwise brightness-normalised mean-absolute-difference:

```
group A: slots 0,1,2,3,6,7,10,11      intra 0.05-0.07
group B: slots 4,5,8,9,12,13,14,15    intra 0.05
between A and B: 0.13-0.16
```

The split is unambiguous — inter-group distance is ~2.5x intra-group — and it is **8/8**, not 4/4.
The natural reading is left-side pair vs right-side pair: the Quest's four cameras are two pairs,
and `notes/22` already found they behave as "two groups of two". Within a pair the baseline is
small and the views nearly coincide, so pixels alone do not separate the two members.

## Why it stops there: the scene is static

```
3999 transitions x 16 slots -> 1382 detected hash changes
expected image writes (27.6 s, 4 cams @ 30 Hz) -> ~3312
detection ratio 0.42
```

With the headset flat on a desk, consecutive frames from the same camera are byte-similar, so more
than half of all writes are invisible to a content hash — and images from *different* cameras are
also similar, because the four views overlap heavily (`notes/15`). Both failure modes have the same
cause. **A longer static capture cannot fix this; only motion can.** Parallax is what separates
cameras, and there is none on a desk.

This is the one place the proximity bypass (`notes/34`) does not help. It grants active tracking, not
a moving scene.

## Consequence for the plan

The remaining work splits cleanly:

- **Unattended, ready now:** everything except camera identity. Pixel access at FrameSet rate works
  (`notes/35`), the descriptor gives per-camera capture timestamps, exposure and gain (`notes/34`),
  and 16 live mappings can be dumped at will.
- **Needs the user, briefly:** identifying which slot is which camera, which falls out immediately
  once the headset moves — parallax separates the pairs, and the stereo baseline separates members
  within a pair.

Worth noting the ask is now much smaller than the "worn 2-minute session" the plan has assumed since
`notes/18`: the headset can be **carried by hand** with the proximity bypass active, and the same
motion serves both camera identification and the step 2 trajectory.

An interim option that needs nobody: any slot from group A paired with any from group B is a
genuine wide-baseline stereo pair, enough to exercise the dataset builder end to end — but not
enough to attach the right calibration, since the extrinsics are per-camera.

## Device / UFS

Restored: `prox_open`, `trackingservice` clean, SELinux Enforcing.

**UFS soak on the stock kernel: 1 h 14 m uptime, 0 reset/UIC errors, no wedge** — through several
trackingservice restarts and ~5 MB of frame dumps. The instrumented kernel had already logged errors
by ~15 minutes in both prior sessions. Suggestive, not yet conclusive.
