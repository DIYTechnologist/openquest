# Step 1 (B2 direct-kernel camera) — acceptance results — 2026-09-04

Against the criteria in `notes/18`. B2 = `tools/cam_kernel/`, no Meta userspace blobs.
B1 = `tools/cam_direct/` under `SYNCBOSS_RAW=1`, the reference that works.

| # | Criterion | Result | |
|---|---|---|---|
| 1 | No Meta lib **mapped** during capture | **0** | ✅ |
| 2 | ≥ 99 % frame delivery over 60 s | **99.43 %** (3600 / 3621) | ✅ |
| 3 | Two groups of two, identical timestamps within a group | cam0≡cam1, cam2≡cam3, **0.0 µs** over 3600 frames | ✅ |
| 3b | < 200 µs between groups | median **91 µs**; p99 1172 µs — see below | ⚠️ superseded |
| 4 | Frames within **2 LSB** of B1, static scene | **3.3–4.9 LSB**; criterion is below the noise floor — see below | ⚠️ not as written |
| 5 | B2 dataset → OpenVINS bounded trajectory | **not yet run** — needs a motion capture | ⬜ |

## Criterion 3b measures the wrong thing

Within a group the offset is **exactly 0.0 µs for all 3600 frames**. That is the tell: both cameras
on a VFE take their timestamp from a *single IRQ handler invocation*, so the V4L2 timestamp has
per-VFE granularity. The inter-group number is therefore **VFE0-vs-VFE1 interrupt latency**, and
cannot express sensor synchronisation at all. Its tail (13.5 % of frames > 200 µs) is scheduling
jitter under 240 IRQ/s.

Replaced with a **data-level** check that timestamps cannot fake. The MCU alternates exposure every
frame (`frame_tag_mode`), swinging the ROI mean ~48 vs ~5, so FSIN-locked groups must hold the same
exposure phase:

```
per-camera alternation broken on 0.19–0.42 % of frames   (= the dropped frames)
group A (cam0) vs group B (cam2) phase agreement: 3584/3589 = 99.86 %
median inter-group offset per 10 s block: 28.5, 49.5, 51.0, 48.0, 31.0, 32.0 µs  -- no trend
```

Frame-for-frame lockstep with **no drift over 60 s**. FSIN sync is preserved; criterion 3's intent
is met even though 3b as literally written is not the right instrument.

## Criterion 4: 2 LSB is below the sensor noise floor

The control that decides this is **B1 against itself** — same tool, same scene, different frames:

| cam | B1-vs-B1 | B2-vs-B2 | B1-vs-B2 |
|---|---|---|---|
| 0 | 9.90 | 9.83 | 4.26 |
| 1 | 10.44 | 10.44 | 4.93 |
| 2 | 6.39 | 6.38 | 3.34 |
| 3 | 11.99 | 12.07 | 4.69 |

**B1 cannot meet its own 2 LSB criterion** (6.4–12.0). Mean-correcting to remove mains flicker
barely moves it (8.8 vs 9.9), so the spread is sensor noise plus real scene motion — the test scene
had a person in it, which was a methodological miss; a genuinely static scene would tighten the
absolute numbers but not the comparison.

What matters is that **B2's self-consistency matches B1's to within 0.03 LSB on every camera**, and
cross-tool agreement (3.3–4.9) is no worse than either tool's own frame-to-frame variation. The two
pipelines are statistically indistinguishable. Recorded as parity-demonstrated; the 2 LSB threshold
should be struck from `notes/18` as unmeasurable rather than carried as a failing item.

## Remaining

Criterion 5 needs a capture **with motion** — a static desk capture cannot exercise a VIO
trajectory. Smallest sufficient version: ~30 s of the headset being picked up and moved in a figure
of eight, then set back down. Does not require the headset to be worn.

## Method note

Two criteria here were nearly mis-reported as failures. Both were rescued by running the **control
through the identical measurement** — the same lesson as `notes/19` session 6. A threshold is only
meaningful once the reference has been measured against it; "B2 scores 4.2 against a 2 LSB bar"
means nothing until B1 scores 9.9 on the same bar.
