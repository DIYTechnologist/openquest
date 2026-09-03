# OS replacement: measured constraints — 2026-09-03

Goal restated by the user this session, which is `notes/01`'s target stack plus one addition:
recent Android (or custom Linux) + our open VR code, as a generic headset, **running any app that
exists as a sideloadable APK** (not Meta Store apps), usable as a SteamVR desktop.
Constraint: *open source we can port is fine; closed binaries are not.*

## Finding: there is no vendor partition. The GSI shortcut does not exist.

The obvious cheap route to recent Android is a Treble GSI: flash a generic AOSP system image on
top of the existing vendor partition, keeping every Qualcomm HAL. **That is not available here.**

```
/vendor -> /system/vendor        symlink, NOT a separate mount
by-name/                         system_a, system_b only -- no vendor_*, no super
ro.product.first_api_level = 25  launched on Android 7.1, upgraded to 10
ro.treble.enabled       = true   set, but meaningless with vendor inside system
ro.boot.dynamic_partitions       empty
ro.vndk.version         = 29
```

The device launched pre-Treble (API 25) and never got a real system/vendor split. `/vendor` is a
directory inside the system partition, so **a GSI would delete the entire vendor HAL layer along
with it** — Qualcomm's msm8998 HALs and Meta's camera/sensor shims together.

`ro.treble.enabled=true` is a trap: it is set, and it does not mean what it usually means. Check
the partition table, not the property.

### Consequence

Any OS replacement means **rebuilding the vendor HAL layer**, so nothing in `/vendor` can be
inherited. That settles a question this project has been equivocating on:
`tools/cam_direct/` currently `dlopen`s `libqcameraoculushal.so`, `libqcameradriver.so` and
`libsyncboss.so` — all closed Meta binaries living in `/system/vendor`. **The tracking pipeline as
it stands does not survive an OS replacement.** The direct-kernel camera path ("B2", notes/11) is
critical path, not an optimisation. We have full kernel source for both the camera pipeline and the
`oculus,camera`/syncboss drivers, so it is scoped work, not research.

## Consequence of "no closed binaries" for app compatibility

- **OpenXR APKs: achievable.** Monado provides an Android OpenXR runtime; an OpenXR-native APK
  loads the runtime through the loader. ALVR is open source and OpenXR, so the SteamVR-desktop
  goal sits entirely inside the achievable half and needs nothing from Meta.
- **VrApi APKs: excluded by the constraint.** They link Meta's closed `libvrapi.so`. Supporting
  them means writing a VrApi→OpenXR shim — new code, not a port, and nobody has written one.
- **Entitlement is mostly a non-issue** for sideloaded APKs; it is a Store mechanism. Some apps
  call the Platform SDK even when sideloaded and will fail regardless.

## The floor: "no Meta blobs" is achievable, "no closed binaries" is not

Restating `notes/01` because it bounds the whole objective. Cannot be removed on this silicon:
Qualcomm **PBL** (mask ROM), **XBL**, Adreno **zap shader** (must be signed), **Hexagon DSP**
firmware, **WCN3990** Wi-Fi firmware.

Honest target: **no closed binaries above the firmware line** — the same nonfree footprint as any
mainline-Linux Qualcomm phone.

## Open decision: Android vs Linux — now settled by the APK requirement

Sideloadable-APK support requires an Android userspace, so Linux-only is out for the app goal.
(Waydroid could in principle run APKs on Linux, but a containerised Android under a Linux host is
a poor bet for VR motion-to-photon latency; not recommended without measurement.)

Recent Android on a 4.4 kernel is feasible in the LineageOS sense — many msm8998 devices run
current LineageOS on 4.4 downstream kernels — so **mainlining the kernel is not a prerequisite for
recent Android**, though it is for the cleanest end state. Meta must release the kernel source
under GPLv2 (`CONFIG_OCULUS_SWD_SYNCBOSS` is a kernel driver), which is the lever for the
`monterey` device tree.

## Unchanged risks

- **Controllers** (`notes/01`: low-medium confidence) — proprietary 2.4 GHz via the SyncBoss MCU,
  nothing done. Least-scoped item, highest chance of an unpleasant surprise, and VR games need it.
- **Display/compositor** — bespoke DSI panel `qcom,mdss_dsi_sdc_lightman`; direct mode, lens
  distortion and reprojection all unbuilt.
