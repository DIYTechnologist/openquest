# Step X — real-time budget on the Quest's own SoC — 2026-09-03

**Question:** does OpenVINS fit the 33.3 ms budget at 30 Hz on the Quest 1's Snapdragon 835?
Everything to date ran on the host, so this was the largest unexamined project risk: step 4
(replacing `trackingservice` in place) is impossible if the answer is no, and the whole
incremental-replacement strategy leans on it.

**Answer: yes, after tuning, with no measurable accuracy cost.** 64.53 ms → **31.75 ms** median,
against a 33.3 ms budget. **Step 4 is not invalidated.**

## Measured

`tools/openvins-android/` cross-builds OpenVINS + a benchmark runner for arm64 Android; the runner
feeds data identically to `euroc_runner` (same `IMU_LEAD_S` look-ahead) so on-device numbers
describe the same work. Dataset: `vio-table2` (1194 stereo frames, 40 s). PNG decode is timed
separately and **not** charged to the estimator — on a real device frames arrive as raw buffers, so
charging PNG decode would overstate the cost by ~9 ms/frame.

| config | median | mean | p90 | p99 | vs budget |
|---|---|---|---|---|---|
| baseline (`num_pts` 200, `max_slam` 50, stereo, full res) | 64.53 | 56.44 | 88.96 | 106.44 | **1.94×** |
| `max_slam` 50→25 | 56.94 | 51.55 | 77.57 | 101.98 | 1.71× |
| mono (1 camera) | 48.51 | 40.45 | 63.39 | 86.14 | 1.46× |
| `downsample_cameras` (half res) | 47.98 | 44.41 | 69.56 | 84.09 | 1.44× |
| `num_pts` 200→100 | 45.79 | 43.53 | 64.35 | 92.03 | 1.37× |
| **combo** (`num_pts` 100 + `max_slam` 25 + half res, stereo) | **31.75** | 31.10 | 42.67 | 56.75 | **0.95×** |

Costed savings from baseline: feature count −29 %, half resolution −26 %, mono −25 %,
`max_slam` −12 %. They compose better than they add: the combo lands at −51 %.

## Accuracy is not traded away

Speed is worthless if the estimate degrades, so each tuned config was re-run on the host and
compared against the converged baseline (`notes/14`):

| config | final ‖p‖ | path length | max speed |
|---|---|---|---|
| baseline | 0.56 m | 11.98 m | 1.80 m/s |
| `num_pts` 100 | 0.55 m | 12.00 m | 1.90 m/s |
| **combo** | **0.54 m** | **12.04 m** | 1.73 m/s |

A 2× speedup for a 2 cm difference in final displacement, on this capture.

## Thermals: clean

Six back-to-back runs, ~5 minutes continuous:

```
median 31.32 -> 31.42 -> 31.89 -> 31.86 -> 31.84 -> 31.81 ms
big cores (cpu4-7) pinned at 2304 MHz for the entire run
```

1.6 % drift across five minutes and **no throttling of the big cores**. The little cores drop to
300 MHz between runs, which is the governor idling them, not thermal limiting.

## Two honest caveats

1. **The median fits; the tail does not.** p90 is 42.7 ms and p99 56.8 ms, both over 33.3 ms. A
   real-time pipeline would need frame dropping, a deadline scheduler, or more headroom — "fits the
   budget" here means the median does, not that every frame does.
2. **This measurement had the SoC almost to itself.** A real VR stack also runs a compositor and an
   application. Landing at 0.95× of budget with nothing else competing is tighter than it looks;
   treat the combo config as the *floor* of what is needed, not comfortable headroom.

Further headroom exists and is untouched: the Hexagon DSP/CDSP (which Meta's own stack offloads CV
work to, `notes/01`), GPU offload for the front end, and dropping to 3 cameras.

## Build notes

No CMake — OpenVINS' build assumes ROS/catkin, so sources are compiled directly. Excluded:
`ov_init/src/ceres/*` and `DynamicInitializer.cpp`, which are the only things pulling in Ceres and
are dead code under `init_dyn_use: false`. Both that and the three `boost::filesystem` symbols are
**stubbed to abort, not to return plausible values** — a benchmark that silently runs a different
configuration than it reports is worse than one that stops. `boost::filesystem::status` is the
exception: it is genuinely reached (config-file existence checks) and gets a real `stat()`
implementation.

`fetch.sh` pulls OpenCV's Android SDK (arm64 static libs — the only non-header dependency), Boost
headers, Eigen, and the OpenVINS source out of the `openvins:runner` image so the on-device numbers
describe the same revision the host runs. ~1 GB, gitignored.

## Status against the notes/18 acceptance criteria

- [x] Median per-frame time measured on-device against the 33.3 ms budget — **64.53 ms baseline,
      31.75 ms tuned**
- [x] Sustained ≥ 5 min without thermal throttling, CPU clocks logged
- [x] Costed list of tuning options with measured savings for each
- Kill criterion (> 3× budget after tuning) **not** triggered — baseline was 1.94× before any tuning
