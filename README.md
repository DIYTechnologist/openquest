# Quest 1 — open VR stack on `monterey`

Reverse-engineering and tooling to replace Meta's proprietary VR blobs with an open stack
(Monado/Basalt) on a **Quest 1** (`monterey`, Snapdragon 835 / msm8998), on an owned device.

> This repo versions **source, notes, and small text/JSON artifacts** only. Large device
> dumps, downloaded toolchains, and pulled vendor binaries are `.gitignore`d — see
> [Regenerating excluded artifacts](#regenerating-excluded-artifacts). Some excluded data
> (`backups/`, factory calibration) is **per-unit and sensitive** (serials, keys); do not
> publish it.

## Status

| Milestone | State |
|---|---|
| Persistent root (Magisk, `LEGACYSAR=true`) | ✅ flashed & verified across reboot |
| Factory calibration export + Basalt/Kalibr converter | ✅ |
| Oculus sensors HAL (`vendor.oculus.hardware.sensors@1.0`) reverse-engineered | ✅ |
| Live enumeration client (getProperties/getChannels) | ✅ struct layouts verified |
| Cross-compile toolchain (NDK + AOSP headers + `__1` ABI fix) | ✅ |
| `hidl-gen` built from source; `ISensorClient` bindings generated | ✅ |
| IMU FMQ streaming client reaches the HAL over binder | ✅ (real transaction) |
| First IMU frame | ⏳ needs exact `sizeof(ImuData)` + `FmqConfig` (see notes/07) |

## Layout

- `notes/` — the primary record. Read in order:
  - `01-recon-findings.md`, `02-owner-admin-adb-access.md`
  - `03-persistent-root-magisk.md` — Magisk boot patch, the `LEGACYSAR` root cause, flash
  - `04-openvr-stack-survey.md` — what Meta ships; where the blobs are
  - `05-calibration-export.md` — factory cam/IMU/mag calibration + the converter
  - `06-sensor-tap-probe.md` — Syncboss (open IMU path) + camera V4L2 + the Oculus HIDL HAL
  - `07-hal-A-camera-imu-interface.md` — the HAL interface, toolchain, hidl-gen, streaming client
- `tools/`
  - `quest_calib_convert.py` — Meta factory calib → Basalt/Kalibr (`exports/.../openvr_calib_out/`)
  - `hal_probe/` — live HAL enumeration client (recovers struct layouts empirically)
  - `hal_stream/` — IMU FMQ streaming client (`hal_stream2.cpp` uses the generated ISensorClient)
  - `hidl-build/` — `build_hidlgen.sh` + reconstructed `.hal` (`iface/`) for generating bindings
- `recon/`, `exports/`, `devicetree/` — text findings, converted calibration, DT dumps
  (large binaries within are gitignored)

## Regenerating excluded artifacts

The build/RE toolchain is downloaded, not committed. To rebuild:

1. **Android NDK r27c** → `tools/android-ndk-r27c/`
   `curl -O https://dl.google.com/android/repository/android-ndk-r27c-linux.zip && unzip`
2. **AOSP Android-10 headers** → `tools/aosp-headers/inc/` (libfmq, libhidl, libcutils,
   libutils, liblog, libhwbinder; from `android-10.0.0_r47` gitiles archives — see notes/07).
3. **hidl-gen** → `tools/hidl-build/`: fetch `system/tools/hidl` + libbase (`system/core/base`),
   `bash tools/hidl-build/build_hidlgen.sh`. Grammar/toolchain fixes are documented in notes/07
   (bison `%define api.pure` removal; flex `YYSTYPE`/`YYLTYPE` shim; `-fno-rtti`; etc.).
4. **Device libs** → `recon/hal-A-2026-08-31/devlibs/`: `adb pull` the runtime `.so`s (list in
   notes/07). Build clients per the `build.sh` in each `tools/hal_*` dir.

## Scope

Owner-authorized work on one owned Quest 1. No third-party systems, accounts, or content
protection are in scope. Reversible throughout (stock `boot_a` preserved in `backups/`).
