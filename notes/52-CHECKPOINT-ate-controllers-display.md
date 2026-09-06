# CHECKPOINT — first ATE, controllers decoded, display characterised — 2026-09-06

Read this first on resume. Supersedes the "next steps" of `notes/24`; the standing rules and traps
in `notes/10` and `notes/24` still hold, plus new ones below.

**Project goal:** replace Meta's blobs on an EOL Quest 1 (`monterey`, msm8998) with an open VR
stack, so the device can run a modern OS and general APKs (ALVR/SteamVR). Strategy (`notes/17`,
`notes/18`): replace Meta's services one at a time on the stock OS, so the eventual OS swap is a
*port of known-working code* with a known-good fallback.

---

## Scoreboard

| # | Step | State |
|---|---|---|
| 0 / X | VIO converges; real-time budget | **DONE** (`notes/14`, `notes/20`) |
| 1 | Direct-kernel camera (B2) | **DONE** — 5/5 (`notes/22`), but see the caveat below |
| 2 | Ground truth vs Meta | **first ATE delivered: 7.6 cm RMSE** (in-place motion). **Room-scale still diverges** (`notes/51`) |
| 3 | Controllers | **input DONE** — every control, both controllers (`notes/49`). 6DoF pose: **not on the MCU** (`notes/50`), fusion location open |
| 4 | `trackingservice` in place | injection proven (`notes/23`); motion-to-photon criterion still open |
| 5 | OS swap | not started (gated on 2, 3, 6B) |
| 6A | Display characterisation | **effectively DONE** — timing, distortion, persistence (`notes/44`, `notes/42`, `notes/46`) |

## The single most important thing learned this session

**Frame timestamps from the leech are 100-250 ms EARLY relative to the IMU/Meta clock, and this is
not a constant.** It was mistaken for a camera↔IMU rotation problem for most of the session
(`notes/48`). It is not. `notes/51` has the full account. Practical consequences:

- Any new dataset must have its camera shift **measured, not assumed**:
  rectify the pair, then run the roll-correlation lag sweep in `notes/51`. Two independent windows of
  the same capture agreed to 12 ms, so the measurement is trustworthy — but two *different captures*
  gave 254 ms and 110 ms, so it is per-session.
- `tools/vio/build_euroc_leech.py --cam-shift-ns` takes it explicitly. The default (254e6) is the
  value from the capture that established the method, **not a universal constant**.
- `check_imu_cam_extrinsic.py` assumes zero lag and will give a confident *wrong* verdict without a
  lag sweep. Filter `|omega|>8 rad/s` outliers too (some affine fits give 68 rad/s).
- **The `R_i_c^T` convention (Basalt/OpenVINS's reading of `T_imu_cam`) is CORRECT** — confirmed
  r=0.893 vs 0.410 for the opposite sense. Stop suspecting the extrinsic.

## What is now true that was not

**A real accuracy number exists.** 7.6 cm ATE RMSE (Sim3/Umeyama), 0.054 m/min drift, 11.0 cm RPE
over 1 s windows, against Meta ground truth captured in the same session (`notes/51`).

**Both controllers are fully decoded** with no Meta userspace code (`notes/49`). Trigger/grip are
twin 12-bit inverted axes in `0x63`; A/X, B/Y, stick-click, menu are bits `0x01/0x02/0x04/0x08` of
`0x24`; thumbstick is `0x82` as 2×int16. **Handedness must be read from the wire** (`byte10` of the
`0x8f` header), never from the device id — ids are per-unit and differ on every headset.
`tools/controller/ctl_decode.py` is the reference implementation.

**The display is characterised** (`notes/44`, `notes/42`, `notes/46`): 71.8209 Hz (not 72), video
mode, dual DSI, **13.718 ms bottom-to-top rolling scanout with only 0.206 ms vblank** — so
reprojection must track scanout phase, it is not a global flash. Meta's distortion mesh is decoded
(`tools/display/decode_distortion_mesh.py`) and converted to a Monado-consumable sampler
(`tools/display/mesh_to_monado.py`). **Low persistence appears to be panel-native** — no software
path drives it, which means a replacement stack inherits it free.

**The proximity bypass removes most "needs a worn headset" constraints** (`notes/34`):
`am broadcast -a com.oculus.vrpowermanager.prox_close` gives full 6DOF tracking with the headset on
a desk. Re-assert it periodically during long captures or it lapses (~9 s otherwise).

## Corrections to earlier notes — do not re-trust these

Several confident claims were measured false this session. Each is annotated at its source, but in
summary:

| claim | status |
|---|---|
| `cam = id & 3` (`notes/10`) | **wrong** — `id` is a frameset ring index; camera is the ctor's position `k` in its run of four (`notes/38`) |
| `ImageBuffer` ctor is per-frame (`notes/10`) | **wrong** — it is a pool-allocation event (`notes/31`) |
| pixel VAs are trackable (`notes/31`, `notes/33`) | **wrong** — `this+0x60` is a *locked* VA, gone after unlock. Map the dmabuf yourself (`notes/35`) |
| camera identification needs motion (`notes/36`) | **wrong** — it needed all 64 buffers (`notes/38`) |
| `0x51` = camera exposure @30 Hz (`sb_decode.py`) | **wrong** — fixed ~29.6 Hz MCU tick, rate-invariant to the camera. `0xe0` is the exposure stamp (`notes/45`) |
| camera runs at 30 Hz | **wrong for the leech path** — 2 interleaved exposure classes × 25 Hz. `cam_kernel` really does run 30 Hz; that is *our* config, not a hardware property (`notes/45`) |
| UFS clock-gating pin prevents the wedge | **wrong** — it died anyway with the pin set (`notes/32`) |
| `notes/22`'s 0.203 m validates the pipeline | **overstated** — that was a 7.24 m path, 0.62 m excursion desk-scale wiggle, not room-scale (`notes/48`) |

## Standing rules (carried forward, plus new)

- **Wait for the user's "go" before any capture needing them**, and **state the timing in the
  message beforehand** — script output is shown to the assistant, *not reliably to the user*. A
  capture was wasted this way (`notes/43`).
- **Pull captures to the host before relaunching anything.** The `ts_ibfs9.sh` launcher used to
  `rm -rf` the capture dir and destroyed a verified 4.7 GB capture. Fixed, but pull first anyway.
- **Check `/tmp` free space before large pulls.** A 4.9 GB pull failed silently at a consistent 63 %
  on both transports because tmpfs was full — it looks like a network fault and is not.
- `adb tcpip 5555` puts adbd in TCP mode, so **USB adb stops working** until `adb usb` or a reboot.
  Re-establish WiFi over USB with `adb -s <serial> tcpip 5555`.
- The **"device is corrupt" boot screen is unavoidable and cosmetic** — no AVB 2.0, secure-boot fuses
  hold Meta's key, ABL is Oculus-signed. Costs one power press per boot; do not chase it
  (`notes/32`, memory `quest-bootloader-cannot-be-signed`).
- Device currently runs **Meta's stock kernel**, reverted from our instrumented build because the
  latter is the prime suspect for the UFS wedges (`notes/32`). Step 1's camera `dynamic_debug`
  needs the instrumented kernel back if that work resumes.

## Immediate next steps

**1. Diagnose the walking capture's residual divergence (offline, no device).** This is what closes
step 2. The timing fix took it from 7562 m to ~2800-3900 m — real but not resolved. Two candidates:

  - **Timestamp jitter, not a constant offset.** Measure the lag in short rolling windows across the
    capture rather than one global fit. If it wanders, a single constant correction would fail
    exactly during fast motion — which is the symptom.
  - **Feature-tracking quality under real motion.** Measure KLT survival and reprojection residuals
    during the walking segments. Faster motion invites blur the in-place capture never stressed.

  Data is on disk: `exports/step2-motion3-2026-09-05/` plus `work/vio-leech/euroc*` (the walking
  dataset's PNGs survive at `work/vio-leech/euroc/`). The phases capture's 4.9 GB blob is at
  `/tmp/frames_p.bin` — **tmpfs, will not survive a reboot**; re-pull from the device or re-capture
  if lost.

**2. Controller IR blobs (offline).** The dim exposure class (`notes/41`) is the likely
controller-tracking exposure. If any capture has a controller in view, look for constellation blobs
— that would answer where 6DoF controller pose is fused (`notes/50` ruled out the MCU).

**3. Needs the user:** a cleaner room-scale capture — but *only after* (1) explains why walking
diverges. And motion-to-photon (6A's last item): `0x55` is display vsync on the IMU clock
(`notes/47`), so timing is solved, but *attributing* an injected pose to a displayed frame is not.

**Do not start** the OS swap (5) or Monado bring-up (6B) yet — both are gated on step 2's room-scale
number and step 3's pose-fusion answer.

## Guardian — asked and answered

The user asked whether Guardian needs implementing. **Recommendation: no, not Meta-style.** For the
ALVR→SteamVR target, SteamVR's own Chaperone provides the safety boundary and needs no
Meta-specific work. Meta's camera-based automatic room scan is a computer-vision project in its own
right and is not warranted. `notes/16` already counted boundary/guardian as 6 of `libvrapi.so`'s 107
exports and judged them safely stubbable. **Safety note:** until Chaperone or equivalent is wired
up, room-scale testing has no collision warning at all.

## Device state at checkpoint

Stock kernel, Magisk root, SELinux **Enforcing**, `trackingservice` clean (no preload).
Wireless adb at `192.168.2.71:5555` plus USB. 41 GB free on `/data`. UFS: 0 errors since the last
reboot (~2 h). Backups at `backups/boot-monterey/` — `new-boot_magisk30.7.img` is what is installed.
