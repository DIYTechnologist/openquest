# Quest 1 Open-VR-Stack Survey

Date: 2026-08-30
Device: Quest 1 / `monterey`, SoC **msm8998 (Snapdragon 835)**, slot `_a`
Captured with persistent Magisk root (see [[03-persistent-root-magisk]]).
Raw data: `recon/openvr-survey-2026-08-30/`

Goal context: replace Meta's proprietary VR blobs with an open stack. This survey inventories
what Meta ships, what talks to hardware, and what device-specific data must be preserved.

## Top-level shape

- 109 packages total; **71 are Meta/Oculus** (`recon/…/packages-meta.txt`).
- The VR runtime is **not** in `/vendor` — the proprietary services live in `/system/bin`
  and are wired up by `/system/etc/init/*.rc`. `/vendor` is mostly stock Qualcomm msm8998
  HALs plus a thin set of `vendor.oculus.hardware.*` HIDL shims.

## Layer 1 — Meta VR services (in `/system/bin`, the real targets)

Running init services (`recon/…/hal-services.txt`) and their binaries:

| init service | binary | role |
|---|---|---|
| `trackingservice` | `/system/bin/trackingservice` | 6DoF inside-out head tracking (SLAM) |
| `calibration_svr` | `/system/bin/calibrationserver` | camera/IMU calibration provider |
| `vrapi_svr` | `/system/bin/vrapiserver` | VrApi compositor/runtime backend |
| `vrfocus` | `/system/bin/vrfocusserver` | app focus / lifecycle arbiter |
| `cameramuxmodeservice` | `/system/bin/cameramuxmodeservice` | tracking-camera mux |
| `sensorservice` | `/system/bin/sensorservice` | (AOSP, but fed by Oculus sensors HAL) |

Supporting CLIs present: `trackingservice_ctl`, `trackinginterface_cli`, `tracked_object_ctl`,
`calibration_manager_ctl`.

## Layer 2 — proprietary libraries (the hard blobs)

Key closed libs (`recon/…/vr-libs.txt`):

| lib | significance for an open stack |
|---|---|
| **`libossdk.oculus.so`** | the Oculus "OS SDK" — inside-out tracking/SLAM core. **The single hardest blob to replace.** |
| `libvrapi.so` | VrApi runtime; apps link against it. Open stack must provide an OpenXR path instead. |
| **`libopenxr_forwardloader.oculus.so`** | an OpenXR forward-loader already ships — meaningful entry point for an OpenXR runtime. |
| `libtrackingengines.so`, `libtrackinginjection-service.so`, `libtrackingutils.so` | tracking pipeline internals |
| `libcalibrationstore.so` | reads the persisted calibration blobs |
| `libqcameraoculushal.so` (vendor) | Oculus camera HAL over the QC camera stack |
| `libOVRMrcLib.oculus.so`, `libxrstreaminghost.oculus.so` | mixed-reality capture / PC streaming |

## Layer 3 — vendor HALs (mostly stock QC msm8998; keep)

`/vendor/lib64/hw` is standard `*.msm8998.so` (gralloc, hwcomposer, audio, keymaster, vulkan)
plus Oculus HIDL shims: `vendor.oculus.hardware.{sensors,graphics.composer,clocks,catty,
devicecert,light,telemetry,wifi.supplicant}@1.0`. The only Oculus-specific display piece is
`vendor.oculus.hardware.graphics.composer@1.1-impl-monterey.so`.

DSP: standard Hexagon `adsp.bNN` firmware in `/vendor/firmware` + `/vendor/lib/rfsa/adsp`
skels. Quest 1 offloads part of the tracking/CV math to the CDSP/aDSP; an open tracker would
either reuse these skels or run CV on the CPU/GPU.

## Layer 4 — device-specific data to PRESERVE (do not lose these)

**`/persist/` (already in gold backup):**
- `calibration/camera_calibration.json`, `camera_calibration_v2.json` — **per-unit factory
  camera intrinsics/extrinsics**. Irreplaceable; any open tracker needs equivalent data.
- `sensors/` — IMU calibration.
- Also: `display/`, `drm/`, `wlan_mac.bin`, `bluetooth/.bt_nv.bin` (radio/identity).

**`/vision/` (already in gold backup):**
- `insideout/mapdb/*.mapdata` — the SLAM map database (regenerates, but shows the format).
- `handtracking/PersonalizationConfig.json`, `mapdata/`, `config/`, `data/`.

Both partitions are in `backups/1PASH9ACHD0215-2026-08-30T15-20-root/`. The camera/IMU
calibration is the crown jewel: without equivalent intrinsics, an open tracker cannot produce
correct 6DoF.

## Realistic open-stack path (for later planning, not yet decided)

1. **Runtime:** Monado is the natural open OpenXR runtime target. The existing
   `libopenxr_forwardloader.oculus.so` shows an OpenXR entry point already exists on-device.
2. **Compositor/display:** needs `vendor.oculus…composer@1.1-monterey` or a direct-mode DRM/KMS
   path on the msm8998 display — the display panel node is known (`qcom,mdss_dsi_sdc_lightman`).
3. **Tracking (the wall):** `libossdk.oculus.so` does inside-out SLAM. Options: (a) keep it as
   a blob behind an open runtime, (b) bring up an open visual-inertial tracker (e.g. Monado's
   built-in / Basalt) using the factory calibration from `/persist/calibration`. (b) is the
   real research effort.
4. **Sensors/cameras:** reuse the QC/Oculus camera + sensors HALs (they're the hardware
   interface); the open tracker consumes their streams.

### Bottom line

The blobs to replace are concentrated in ~6 `/system/bin` services and a handful of
`*.oculus.so` libs — **not** scattered through `/vendor`. The vendor HAL layer is largely
stock Qualcomm and can stay. The hardest single dependency is inside-out tracking
(`libossdk.oculus.so` + factory calibration); everything else (runtime, compositor, input)
has a plausible open path via Monado/OpenXR.
