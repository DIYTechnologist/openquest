# Execution plan — measurable milestones — 2026-09-03

Strategy (`notes/17`): replace Meta's services one at a time on the stock OS, so the OS swap
becomes a **port of known-working code** with a known-good fallback. Every replacement is built as
**portable core + thin Meta adapter**; only the adapter is throwaway.

Each step below has **acceptance criteria that are falsifiable** — a number or a binary condition —
plus kill criteria, so we can tell forward progress from motion. Steps needing the headset are
marked ⚠; per the standing rules, prompt and wait for "go".

---

## Scoreboard

| # | Step | State | Headline metric | Now |
|---|---|---|---|---|
| 0 | Open VIO converges | **DONE** | drift on 23 s capture | 0.56 m |
| 1 | Direct-kernel camera (B2) | **DONE** — 5/5 (`notes/22`), final ‖p‖ 0.203 m | Meta libs needed by capture | **0** |
| 2 | Ground truth vs Meta | poses+IMU **captured & cross-validated r=0.997**; frames blocked by the ImageBuffer **pool** (`notes/31`) | ATE RMSE vs Meta poses | unknown |
| 3 | Controllers | **stream found**: enable=213, data=0x8f, IMU decoded @501 Hz (`notes/27`) | button decode agreement | 0 % (needs presses) |
| 4 | `trackingservice` in place | **task 1 DONE** — pose injection works (`notes/23`) | Meta shell on our poses | pose accepted, compositor unverified |
| 5 | OS swap | not started | boots + tracks + streams | no |
| 6 | Display/compositor | **added 2026-09-04** — characterise stock while it exists | latency + distortion reproduced | not started |
| X | Real-time budget | **DONE** (`notes/20`) | VIO ms/frame on-device vs 33.3 | **31.75** (tuned) |

**Checkpoint 2026-09-04: `notes/24`.** Steps 0, X and **1** are done; step 4's shortcut is proven
(`notes/23`). Critical path is now **4 → 5**, all unattended until one worn session serves 2 and 3.

---

## Step 1 — Direct-kernel camera path (B2)

**Objective.** Drive all four cameras with **zero Meta userspace blobs**. Retires
`libqcameraoculushal.so`, `libqcameradriver.so`, `libsyncboss.so`.

**Why first.** `notes/16`: no vendor partition, so the OS swap deletes `/vendor`. This is the one
component where "build on the old OS, port later" only works if we go to the kernel. Also far
easier now, with B1 present to diff against.

**Why it is cheaper than it looks** (all established in `notes/11`):
- **No GPL gap.** `oculus,camera` is published: `drivers/staging/oculus/mcu/syncboss/syncboss_camera.c`.
  Full tree in `work/oculus-kernel/`.
- **The OV7251 register tables are not needed.** Sensor power-up and CCI/I2C init happen in-kernel
  via the `msm_sensor_init` subdev probe. This was the single biggest feared RE cost and it is gone.
- **The ioctl set is already enumerated**: `VIDIOC_MSM_CSIPHY_IO_CFG`, `VIDIOC_MSM_CSID_IO_CFG`,
  `VIDIOC_MSM_ISPIF_CFG{,_EXT}`, `VIDIOC_MSM_ISP_{INPUT_CFG,REQUEST_STREAM,CFG_STREAM,AHB_CLK_CFG}`,
  plus stock `S_FMT/S_PARM/REQBUFS/QBUF/DQBUF/STREAMON`. All in the published tree.
- **MCU control is partly done already.** `cam_direct` already emits a raw type-41 write to
  `/dev/syncboss0` (`raw syncboss CAMERA_RELEASE (type 41) -> write 3` in the capture log).
  Type 40 = power on, 41 = off; `/dev/syncboss_control0` is the open channel from the IMU work.

**Tasks**
1. ~~**1.1 Reference trace.**~~ **DONE** — `tools/cam_kernel/`, 493 ioctls fully decoded,
   `notes/19`. Confirmed the whole path is published kernel ABI: **no unknown-ABI blocker for B2.**
2. ~~**1.2 MCU control open.**~~ **DONE** — `SYNCBOSS_RAW=1` in `cam_direct` drives the MCU with
   our own packets; `libsyncboss.so` is never dlopened and all 4 cameras stream. Frame means match
   the blob path to <0.3 LSB and FSIN pairing is preserved.
3. **1.3 Pipeline open.** Reimplement the `libqcameradriver.so` role: CSIPHY → CSID → ISPIF → ISP
   config, then `REQBUFS`/`QBUF`/`DQBUF`/`STREAMON` buffer pumping, against the published headers.
4. **1.4 Parity.** All 4 sensors, 30 Hz, FSIN-synced, 640×481 mono8.

**Acceptance criteria**
- [x] `grep -ci oculus /proc/self/maps` during capture = **0** — measured 0 (`notes/22`)
- [x] ≥ **99 %** frame delivery over a 60 s run — **99.43 %**
- [x] FSIN sync preserved: two groups of two, byte-identical within a group — **0.0 µs**, and
      inter-group **exposure-phase agreement 99.86 %** with no drift over 60 s. NOTE: the original
      "< 200 µs between groups" sub-criterion measures VFE IRQ latency, not sensor sync, and is
      superseded by the phase check (`notes/22`).
- [~] ~~Frames within **2 LSB** of B1 on a static scene~~ — **struck as unmeasurable**: B1 scores
      6.4–12.0 LSB against itself. Parity shown instead — B2 self-consistency matches B1's to
      within 0.03 LSB (`notes/22`).
- [x] A dataset built through `build_euroc_direct.py` from B2 frames drives OpenVINS to a
      **bounded** trajectory — **final ‖p‖ 0.203 m**, path 7.24 m, 621 poses @ 30 Hz (`notes/22`)

**Kill criteria.** An ioctl or config path is required that is *not* in the published tree, or the
ISP requires an opaque firmware/config blob we cannot construct. If hit: fall back to B1 for the
stock OS and re-scope the camera work as part of the mainline-kernel effort instead.

**Needs headset:** device runs only, no handling. Not marked ⚠.

---

## Step 2 — Ground truth against Meta's tracker

**Objective.** Turn "converged and self-consistent" (`notes/14`) into an **accuracy number**.

**Why now.** Meta's production tracker runs on the same rig and sensors and its output is readable —
this is the absolute reference `notes/14` concluded we lacked:
```
trackinginterface_cli getHeadTrackingData [prediction_ms] [json]     # shared memory
dumpsys tracking                                                     # pose, vel, accel
```

**The blocker to solve first.** `cam_direct` needs `trackingservice` **stopped** (`/dev/video0` is
single-open), so Meta's poses and our raw sensors cannot be captured simultaneously by that route.
Options, in order of preference:
1. Revive the in-process **leech** (`notes/09`, `notes/10`), which read trackingservice's shared
   dmabufs while it ran. Abandoned for lossy frame↔timestamp linking; the "ATOMIC hook" in
   `notes/08` was the intended fix. A fraction of frames is sufficient for drift comparison.
2. Non-simultaneous (same route walked twice) — much weaker; use only if 1 fails.

**Tasks**
1. ⚠ Confirm the `getHeadTrackingData` JSON schema with the headset **worn** (it returned `{}` on a
   desk: proximity gates tracking to STANDBY/0DOF).
2. Build a pose logger sampling Meta at ≥ 30 Hz with timestamps on the tracking clock.
3. Revive the leech to capture frames + IMU *while* trackingservice runs. **IMU DONE** — needs no
   interposition at all, because `/dev/syncboss_stream0` is a multi-reader broadcast fifo and was
   never single-open (`notes/30`, lossless at 994 Hz). **FRAMES BLOCKED** — the `ImageBuffer` ctor
   is a *pool allocation* event, not per-frame, so the leech yields ~96 frames per session, not
   ~9000 (`notes/31`). Fix identified: learn `slot -> pixel VA` at ctor time, then read pixels on
   each `MessageQueue<FrameSet>::read()` using the slot index in `w2`/`w14`.
4. ⚠ **Worn session 1 done 2026-09-04** (`notes/31`): 160 s, Meta poses @ 59.7 Hz + IMU @ 993.6 Hz,
   cross-validated at **r = 0.997**. Frames insufficient; one more worn session needed after the fix.
4. ⚠ Worn capture, ≥ 2 minutes, including translation and fast rotation.
5. Compare: time-align, then ATE/RPE against Meta.

**Acceptance criteria**
- [x] Meta poses logged at ≥ 30 Hz for ≥ 120 s with < 1 % dropped samples — **59.97 Hz for
      125 s, 0.067 % late, 0 read errors** via `tools/pose_log/` (`notes/28`)
- [ ] Our VIO runs on frames captured **in the same session** as those poses
- [ ] **ATE RMSE reported** with a stated alignment method (this is the deliverable — a number, not
      a threshold to pass)
- [ ] **Drift rate in m/min** over ≥ 2 minutes — currently completely unknown
- [ ] Relative pose error over 1 s windows, to separate local accuracy from slow drift

**Kill criteria.** If the leech cannot link ≥ 20 % of frames cleanly, drop to option 2 and label the
result as indicative only.

**Needs headset:** ⚠ yes, worn, ~2 min.

---

## Step 3 — Controllers

**Objective.** Decode controller tracking and buttons ourselves. `notes/01` rates this our
**lowest-confidence** area, and VR games require it — highest chance of an unpleasant surprise, so
find out early.

**Leads.** `trackinginterface_cli getcontrollertrackingdata / getcontrollerbuttondata [json]` gives
a reference. `vendor.oculus.hardware.sensors@1.0::IControllerProvider` is the HAL seam. Controllers
reach the SoC via the **SyncBoss MCU** (proprietary 2.4 GHz radio on the MCU side), and we already
own `/dev/syncboss_stream0`.

**Tasks**
1. ⚠ With a controller paired and awake, dump `getcontrollertrackingdata` / `getcontrollerbuttondata`
   to establish the reference format.
2. Identify controller packet types in the syncboss stream (we already decode type 0x50 IMU); look
   for types carrying button/IMU/pose payloads.
3. Decode buttons first (discrete, trivially verifiable), then controller IMU, then pose.
4. Determine whether controller **pose** is fused on the MCU, in `trackingservice`, or from camera
   IR blobs — this decides whether we must implement constellation tracking ourselves.

**Acceptance criteria**
- [ ] Every button/trigger/thumbstick decoded from the raw stream matches Meta's reported state
      **100 %** over ≥ 50 discrete events
- [ ] Controller IMU decoded, with rate and units confirmed against Meta's reported values
- [x] **Documented answer** to where 6DoF controller pose is computed — **not on the MCU**: the
      full raw stream, captured while both controllers were actively moved, contains no packet
      type and no sub-record with the shape of a pose (`notes/50`). Fusion location within
      `trackingservice` vs. camera-based constellation tracking remains open; the kill criterion
      below is now the live scenario for full 6DoF.
- [ ] If camera-based: quantified — how many IR blobs per frame in the short-exposure frames we
      currently discard

**Kill criteria.** If pose fusion is entirely inside `libtrackingengines.so` with no usable
intermediate, controller 6DoF becomes its own research project; record and de-scope to 3DoF +
buttons for the first milestone.

**Needs headset:** ⚠ yes, plus a paired controller.

---

## Step 4 — Replace `trackingservice` in place

**Objective.** Meta's own shell and compositor running on **our** poses, on the stock OS. The single
most convincing proof the core is correct.

**Prerequisites:** steps 1 and 2, plus the real-time budget (step X).

**Tasks**
1. Probe `TrackingDataInjection`
   (`oculus.internal.virtual_input.ITrackingDataInjectionService`) — if it accepts external poses,
   it is a large shortcut and needs no RE of the producer side.
2. If not: enumerate `oculus.internal.tracking.ITrackingService` transactions and implement the
   adapter, registering under the same name.
3. Run our VIO as a daemon at frame rate, feeding poses in.

**Acceptance criteria**
- [x] `dumpsys tracking` reports **our** poses, `Valid: Yes`, `Tracking Level: 6DOF` (`notes/23`)
- [ ] Meta's shell renders and responds to head motion for ≥ 10 minutes without losing tracking
- [ ] Motion-to-photon latency measured and within **2×** of stock (method stated)
- [x] Zero crashes over a 10-minute session — **0 restarts of `trackingservice`/`vrshell`,
      ~18,000 poses at 30 Hz, 0 injection failures** (`notes/26`). NOTE: `com.oculus.systemdriver`
      does not exist on this device; criterion restated against the processes that do.

**Kill criteria.** If neither injection nor the Binder interface is practical, skip in-place
replacement — it is throwaway adapter work — and go straight to Monado.

**Needs headset:** ⚠ yes, worn.

---

## Step 5 — OS swap

**Objective.** Newer Android + our ported cores.

**Do not start until** steps 1–4 have shipped their cores, because `notes/16` established the swap
deletes `/vendor` entirely, so nothing there is inheritable.

**Tasks**
1. Meta GPLv2 kernel source → `monterey` device tree (`CONFIG_OCULUS_SWD_SYNCBOSS` is the lever).
2. AOSP/LineageOS for `monterey`. Recent Android on a 4.4 kernel is feasible in the LineageOS
   sense — mainlining is **not** a prerequisite, though it is the cleaner end state.
3. Rebuild the vendor HAL layer (unavoidable — no vendor partition).
4. Port the cores from steps 1–4. Adapters are discarded here by design.
5. Monado as OpenXR runtime; ALVR as the first real workload.

**Acceptance criteria**
- [ ] Boots to a usable shell on the newer OS
- [ ] Our camera + IMU stack streams (step 1 core, unmodified)
- [ ] Our VIO produces 6DoF at frame rate (step 4 core, unmodified)
- [ ] Monado passes `hello_xr`
- [ ] **ALVR streams SteamVR with tracked head motion** — the actual end-user goal
- [ ] Meta blobs remaining: **0** above the firmware floor (PBL/XBL/zap/DSP/Wi-Fi remain)

**Needs headset:** ⚠ yes, and it is the first step that risks an unbootable device — full backup and
a verified recovery path before flashing.

---

## Step 6 — Display and compositor

**Added 2026-09-04.** Previously implicit inside step 5 task 5 ("Monado as OpenXR runtime"), which
hid a large workstream behind three words. `notes/16` already listed it under *unchanged risks*:
"direct mode, lens distortion and reprojection all unbuilt".

**Why it is promoted, and why part of it is due NOW rather than at step 5.**

1. **Step 4 already depends on it.** Step 4's criterion is motion-to-photon latency "within 2× of
   stock", which cannot be evaluated without characterising the stock compositor.
2. **The reference is perishable.** `notes/23` proved we can drive Meta's compositor with a known
   pose, which turns it into a *measurable* reference: inject a step change, observe when photons
   change. After the swap deletes `/vendor`, that reference is gone. This is the same logic that
   motivates the whole incremental strategy — build against the known-good while it still runs.

**What exists already:** per-panel display calibration, screen offsets and uniformity are exported
(`notes/05`). Panel is `qcom,mdss_dsi_sdc_lightman`, **2880x1600 @ 72 Hz**. The composer HAL blob
(`vendor.oculus.hardware.graphics.composer@1.1-impl-monterey.so`) is thin — 24 exports; the real
compositing lives in `vrapiserver`/`libvrapi.so`.

**Tasks (A = do now on the stock OS, B = after the swap)**

1. **(A)** Measure stock **motion-to-photon latency** using injection as the stimulus — a known pose
   step at a known time, against photon change. Serves step 4's criterion directly.
2. **(A)** Characterise panel timing: refresh, persistence/low-persistence strobing, vsync
   behaviour, whether the stock path is direct-mode/front-buffer.
3. **(A)** Validate we can **reproduce Meta's lens distortion** from `notes/05` calibration —
   offline is sufficient (render a grid, compare against what the stock compositor produces).
4. **(B)** Bring up the DSI panel under DRM/KMS on the new OS.
5. **(B)** Monado compositor: distortion mesh from our calibration, reprojection/timewarp, direct
   mode.

**Acceptance criteria**
- [ ] Stock motion-to-photon latency **measured**, with the method stated (this is the number step 4
      is scored against, so it is a deliverable in itself)
- [ ] Panel timing documented: refresh, persistence, vsync, direct-mode or not
- [ ] Our distortion correction reproduces Meta's to a **stated pixel error** on a test pattern
- [ ] (B) Monado renders through the panel at 72 Hz with reprojection

**Kill criteria.** If the panel cannot be driven without a Meta display blob, the OS swap keeps a
closed component — record it explicitly against the "0 Meta blobs" goal rather than quietly.

**Needs headset:** tasks 1–3 are device-runs plus one worn session for latency perception; ⚠ partial.

---

## Step X — Real-time budget (cross-cutting, do early)

**Objective.** Retire the largest unexamined project risk: nothing has ever run on the Quest's own
CPU. If OpenVINS cannot hit frame rate on a Snapdragon 835, step 4 is impossible and the
architecture needs rethinking — better to know before building on it.

**Tasks**
1. Cross-compile OpenVINS + our runner for arm64 Android.
2. Run the existing `vio-table2` dataset on-device, timing per frame.
3. Profile: front-end vs update vs marginalisation.

**Acceptance criteria**
- [ ] **Median per-frame time measured on-device**, against the 33.3 ms budget at 30 Hz
- [ ] Sustained run without thermal throttling over ≥ 5 minutes, with CPU clocks logged
- [ ] If over budget: a costed list of options (lower `max_slam`, fewer features, downsampling,
      DSP/GPU offload) with measured savings for each

**Kill criteria.** If > 3× budget even after tuning, escalate: either a lighter estimator or DSP
offload becomes its own workstream.

**Needs headset:** device runs only.

---

## Dependencies and parallelism

The plan is **not** a chain. Only two real dependency edges exist; the rest is parallelisable.

| Step | Hard depends on | Blocks | Notes |
|---|---|---|---|
| **1** B2 camera | *nothing* — B1 is the reference to diff against | 5 | Independent of 2, 3, X |
| **2** Ground truth | *nothing* — uses the **leech**, not `cam_direct` | — | See below; commonly mis-assumed to need 1 |
| **3** Controllers | *nothing* — syncboss stream is already ours | 5 (partially) | Button/IMU decode needs no cameras |
| **X** Real-time | *nothing* — runs the existing `vio-table2` dataset | **4** | Cheap, and can invalidate 4 |
| **4** `trackingservice` | **X**, plus *a* camera path (B1 is fine) | 5 | Does not need B2 |
| **5** OS swap | **1**, **3**, **4**'s core, **6B** | — | Needs kernel-based camera; `/vendor` is gone |
| **6** Display/compositor | 6A: *nothing* (needs stock alive) | 4's latency criterion, 5 | **6A is perishable — the reference dies with the swap** |

```
        ┌── 1 (B2 camera) ─────────────────────────┐
        │                                          │
        ├── 3 (controllers) ───────────────────────┤
start ──┤                                          ├──> 5 (OS swap)
        ├── X (real-time) ──> 4 (trackingservice) ─┘
        │
        └── 2 (ground truth) ──> [validation only, blocks nothing]
```

**Two non-obvious points, both of which unlock parallelism:**

1. **Step 2 does not depend on step 1.** It deliberately uses the leech path — which reads frames
   *while `trackingservice` runs* — precisely because Meta's poses and our sensors must be captured
   simultaneously. `cam_direct` (and therefore B2) is the wrong tool for step 2 by construction.
2. **Step 4 does not depend on step 1.** It needs *a* camera path, and B1 already works. B2 matters
   for the OS swap (step 5), not for the in-place demo.

**Critical path: X → 4 → 5**, with **1** and **3** running alongside as independent long poles.
Step 1 is likely the longest single item, so starting it early matters more than sequencing it first.
Step 2 is off the critical path entirely — it is validation, and blocks nothing.

### Parallel tracks

| Track | Work | Device need | Can start |
|---|---|---|---|
| **A** | Step 1: ioctl trace → MCU control → pipeline reimplementation | runs only, no handling | **now** |
| **B** | Step X: cross-compile OpenVINS arm64, time on-device | runs only | **now** |
| **C** | ~~Step 2 prep: revive the leech, build the Meta pose logger~~ **DONE** (`notes/29`, `notes/30`) | runs only | complete |
| **D** | Step 3 prep: syncboss stream survey for controller packet types | runs only | **now** |
| **E** | Steps 2 + 3 capture: one worn session | ⚠ worn, controllers | after C and D |
| **F** | Step 4: injection probe → daemon | ⚠ worn | after B |

Tracks A–D have **no prerequisites and no dependencies on each other** — all four can proceed
concurrently today. Only E and F need to wait, and E waits on tooling (C, D) rather than on any
result.

### Suggested order for a single worker

**A and B first (interleaved), then C+D, then one E session, then F, then 5.**

Rationale: step 1 (A) is the longest pole and structural; X (B) is cheap and can invalidate step 4,
so pay for it before building there. C and D are tooling for the one expensive user-in-the-loop
event, so they should be ready before it, not after.

**Efficiency note.** Captures are the expensive, user-in-the-loop operation, so **E should be a
single session** serving both steps 2 and 3: worn, ≥ 2 min, controllers paired and exercised,
correct exposure (`e3000_g160` for a bright room — `notes/15`; the `e8000_g255` every capture has
used saturates 11–47 % of pixels), and 4-camera if step 1 has landed by then.
