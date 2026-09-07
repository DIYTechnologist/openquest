# Quest 1 Factory Calibration Export

Date: 2026-08-30
Device: `1PASH9ACHD0215` / `monterey` (msm8998)
Exported with persistent Magisk root ([[03-persistent-root-magisk]]) for open-tracker work
([[04-openvr-stack-survey]]). This is the irreplaceable per-unit crown-jewel data.

## Export location & integrity

```text
exports/calibration-2026-08-30/
  calib_export.tar   SHA-256 4c4697901b4b4392723ad018fe15a760e931463df53c43e628c366a27a26f3e4
  calibration/       (extracted)
  sensors/           (extracted)
```

Tarred from `/persist/{calibration,sensors}` on-device; host tar hash verified equal to the
device-side hash; device temp copy removed. `/persist` is also in the gold backup
`backups/1PASH9ACHD0215-2026-08-30T15-20-root/`, so this is a second, parsed copy.

## Contents & validation

All files parse; all serials = `1PASH9ACHD0215`. This is a coherent VI-SLAM calibration in a
standard form an open tracker (Monado / Basalt / Kalibr) can consume.

### Cameras — `camera_calibration_v2.json`

4× **OV7251** global-shutter, 640×480. Each entry has:
- `DeviceFromCamera` — 4×4 SE3 extrinsic (camera pose in common Device frame)
- `Projection`: `PinholeSymmetric`, coeffs `[f, cx, cy]`
- `Distortion`: **`Fisheye62`** (6 radial + 2 tangential) — maps to Kannala-Brandt / Kalibr
  fisheye models
- `ImageSize`, `Shutter=Global`, optional `ImageMask`

(`camera_calibration.json` is the older/smaller v1; keep both.)

### IMU — `imu_calibration.json`

- `DeviceFromImu` — 4×4 SE3 extrinsic (→ cam-IMU transform via the camera extrinsics, exactly
  what Basalt needs)
- `Gyroscope` / `Accelerometer`: `Model=Linear`, 3×3 `RectificationMatrix` + `Offset`

### Magnetometer — `mag_calibration.json`

- `offset` [3], `rectification_matrix` [9]

### Display (per-panel, for compositor correction — not tracking)

- `calibration/display/{left,right}/`:
  - `<panelSN>.mura` (6,912,021 B each) — per-panel mura/luminance correction map
  - `<panelSN>.uniformity` (96 B), `*_screen_offset.json`
  - Panel SNs: left `1TJ3508D2G0162`, right `1TJ35082QJ0162`

### Online-refined calibration

- `calibration/online/12695210652037270177` (8839 B, dated 2026-08-30) — runtime-refined
  extrinsics the tracker updates online. Snapshot only; regenerates. Factory JSON above is the
  authoritative source of truth.

### Sensors registry

- `sensors/sns.reg` (25468 B) — Qualcomm SLPI sensor registry (binary)
- `sensors/aon_enable`, `sensors/sensors_settings`

## Significance

The hardest dependency for an open stack is inside-out tracking, and its irreplaceable input —
factory camera + IMU intrinsics/extrinsics — is now confirmed **present, complete, parseable,
and in a convertible form**. All extrinsics share one `Device` frame, so producing a
Basalt/Kalibr cam-IMU config is a format conversion, not a recalibration.

## Converter (DONE) — `tools/quest_calib_convert.py`

Converts the factory JSON into open-tracker configs. numpy-only (no scipy/pyyaml).

```sh
python3 tools/quest_calib_convert.py \
  exports/calibration-2026-08-30/calibration \
  -o exports/calibration-2026-08-30/openvr_calib_out
```

Outputs (`exports/calibration-2026-08-30/openvr_calib_out/`):

| File | Purpose |
|---|---|
| `intermediate.json` | **loss-less** — exact Fisheye62 + exact transforms; source of truth |
| `basalt_calibration.json` | Basalt calib, KB4 intrinsics, `T_imu_cam` as quaternion+pos |
| `kalibr-camchain-imucam.yaml` | Kalibr camchain, pinhole-equidistant(KB4), `T_cam_imu` + `T_cn_cnm1` |

### The one approximation, quantified

Basalt/Kalibr can't ingest Meta's **Fisheye62** (6 radial + 2 tangential). Closest is
**Kannala-Brandt KB4** (4 radial, radial-only). The tool fits KB4 to the Fisheye62 radial
mapping over the true FoV (linear least squares) and **measures** the error:

| cam | sensor | FoV | KB4 rms | KB4 max | tangential (dropped) |
|---|---|---|---|---|---|
| 0 | OV7251 | 177.1° | 0.085 px | 0.43 px | 0.36 px |
| 1 | OV7251 | 182.1° | 0.277 px | 1.63 px | 0.27 px |
| 2 | OV7251 | 178.1° | 0.104 px | 0.33 px | 0.65 px |
| 3 | OV7251 | 181.5° | 0.228 px | 1.29 px | 0.83 px |

Worst-case total (KB4 radial + dropped tangential) ≈ **2.4 px**, sub-pixel typical — usable to
bootstrap a tracker. For full fidelity, implement Fisheye62 natively from `intermediate.json`.

### Transforms verified (exact, not approximate)

- Factory rotation matrices orthonormal to `~1e-15`, `det=+1.0`.
- Round-trip check: `DeviceFromImu @ T_imu_cam[i] == DeviceFromCamera[i]` to `0.0` error.
- `T_imu_cam[0]` translation ≈ `[-0.096, -0.006, -0.002]` m — sane camera-from-IMU offset.
- Cameras 0/2 and 1/3 form the two splayed stereo pairs (standard Quest 1 layout).

## Next candidate step

Identify how to tap the 4 camera streams + IMU for an open tracker (camera HAL / sensor HAL
access; see [[04-openvr-stack-survey]] Layer 3). Calibration side is now solved.
