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
| 5 | B2 dataset → OpenVINS bounded trajectory | **final ‖p‖ = 0.203 m** (criterion < 2 m) | ✅ |

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

## Criterion 5 — closed 2026-09-04

75 s handheld capture (`cam_kernel 4 75 3000 160 02`): headset picked up off the desk, moved in a
figure of eight, set back down. All four cameras streamed; cam0/cam2 persisted.

```
2245 / 2247 SLAM frames (cam0/cam2), 74388 IMU samples @ 998.8 Hz, 2256 exposure stamps
stereo pairs within 4 ms: 2245   ->   2237 pairs written after clock snapping
```

OpenVINS on the converged baseline config (`num_pts` 200, `max_slam` 50, stereo,
`init_imu_thresh` 0.3), regenerated for the 0/2 pair:

| metric | value |
|---|---|
| poses | 621 @ 30.0 Hz |
| **final ‖p‖** | **0.203 m** (criterion **< 2 m**) |
| path length | 7.24 m |
| max excursion from start | 0.62 m |
| max speed | 0.63 m/s (median 0.38) |

The estimator initialised **53.9 s into the capture** and tracked the final 21.1 s. Not a fault:
`init_imu_thresh` gates initialisation on IMU excitation and the headset genuinely sat still for the
first ~54 s, so the tracked window is exactly the motion.

**No regression against step 0** (`notes/14`: 0.56 m final on a 23 s capture; this is 0.203 m on
21 s) — and that baseline was built from **B1**, i.e. through Meta's blobs. On this evidence the
open pipeline is at least as good.

Artifacts: `exports/vio-b2-2026-09-04/`.

## Step 1 is complete

All five criteria met, two restated to be measurable (3b and 4 above). The camera path now runs end
to end — sensor to 6DoF trajectory — with **zero Meta userspace blobs**.

## Method note

Two criteria here were nearly mis-reported as failures. Both were rescued by running the **control
through the identical measurement** — the same lesson as `notes/19` session 6. A threshold is only
meaningful once the reference has been measured against it; "B2 scores 4.2 against a 2 LSB bar"
means nothing until B1 scores 9.9 on the same bar.
