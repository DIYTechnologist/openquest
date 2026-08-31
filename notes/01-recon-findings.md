# Quest 1 (`monterey`) — Owner Device-Porting Recon Findings

Date: 2026-08-30
Device serial: `1PASH9ACHD0215`
Goal: replace vendor binary components with an open-source VR stack for an owned,
vendor-unsupported Quest 1, reducing e-waste and restoring maintainable security updates.

## Authorization and scope

This work is owner-authorized repair, backup, preservation, and operating-system porting on
one physical headset: serial `1PASH9ACHD0215`. The scope is local device maintenance only:
reading and backing up partitions, preserving the bootloader-unlock recovery path, and
building an open Linux/VR userspace for this hardware.

Out of scope: Meta services, third-party devices, third-party networks, accounts,
credentials, content protection, and remote access to third-party systems. Security terms
below refer only to owner-authorized access on this unsupported local device.

Terminology for handoff: when asking another tool about the next maintenance step, describe
the need as **temporary owner-admin ADB access for partition backup on my owned Quest 1**.
That is the operational requirement: a local `uid=0` Android shell long enough to preserve
partitions before OS porting work.

## Device identity

| Field | Value |
|---|---|
| Codename | `monterey` (Quest 1) |
| SoC | **APQ8098** (Snapdragon 835, *modem-less* variant) |
| CPU | 4x Cortex-A73 (`0x801`) + 4x Cortex-A53 (`0x800`) |
| Platform | `msm8998` |
| Android | 10 (SDK 29), build `QQ3A.200805.001` |
| Fingerprint | `oculus/vr_monterey/monterey:10/QQ3A.200805.001/49845030443200410:user/release-keys` |
| Kernel | **4.4.205-perf+**, built 2024-07-31 |
| Security patch | 2024-07-05 (final; no further updates ever) |
| Bootloader | **UNLOCKED** — `ro.boot.flash.locked=0`, `verifiedbootstate=orange` |
| Active slot | `_a` |
| Persistent admin shell | **No** — `user` build; temporary owner-admin path verified separately |

### Support/security posture
Linux 4.4 went EOL upstream in **Feb 2022**. The device runs a kernel with ~4.5 years of
unbackported upstream fixes and no vendor support. Mainlining is the only route to a patched
kernel on this hardware.

### APQ vs MSM
`APQ8098` has **no modem**. The entire RIL/modem blob stack is out of scope. Maps onto the
existing postmarketOS `msm8998` port.

## Partition layout

Standard Qualcomm A/B (`_a`/`_b`) layout. **Meta-specific partitions:**

- `ovrtz_a`/`ovrtz_b` — Oculus's *own* TrustZone app, separate from stock `tz_a`/`tz_b`.
- `vision` (`sda9`) — **highest-value target.** Almost certainly Insight SLAM calibration
  (per-unit camera intrinsics/extrinsics). Cannot be recreated from a generic dump — this data
  is unique to this physical unit. **Dump before any destructive operation.**
- `splash`, `persist`, `private`

**Absent:** no `dtbo` partition and no `vendor` partition.
=> DTB is appended to the boot image; vendor content lives inside `system`.

## Access boundary (as `shell`, unprivileged)

| Target | Readable? |
|---|---|
| `/system/**` | YES |
| `/vendor/**` | NO (not even listable) |
| `/proc/config.gz` | YES (pulled) |
| `/proc/device-tree/*` properties | NO (SELinux) |
| `/sys/firmware/fdt` | NO |
| `/dev/syncboss*` | NO (SELinux `syncboss_device`; DAC would allow, MAC does not) |
| block devices / partitions | NO |

## Kernel config discoveries

```
CONFIG_OCULUS=y
CONFIG_OCULUS_MCU=y
CONFIG_OCULUS_SWD=y
CONFIG_OCULUS_SWD_SYNCBOSS=y     <-- key finding
CONFIG_VS1_BOARD=y
CONFIG_REGULATOR_OVR_OLED_AVDD=y
CONFIG_QCOM_KGSL=y               <-- downstream Adreno; replace w/ DRM/MSM + Freedreno
CONFIG_MSM_CAMERA=y              <-- downstream camera; replace w/ mainline CAMSS
CONFIG_QCA_CLD_WLAN_PROFILE="default.oculus"
```

Config also lists sibling codenames (`hollywood`=Quest 2, `seacliff`=Quest Pro,
`eureka`=Quest 3, `panther`, `starlet`, ...) confirming a unified Meta kernel tree.

### SyncBoss — the MCU
`SWD` = Serial Wire Debug. A **separate MCU** (the `oculus,vs1` DT node) is flashed over SWD
*by the main kernel at runtime*. It owns IMU sampling, camera/display sync, and the
proprietary 2.4 GHz controller radio.

Live and active:
```
/dev/syncboss0  /dev/syncboss_control0  /dev/syncboss_stream0  /dev/syncboss_powerstate0
162:  100391  msmgpio  10 Edge  syncboss0    <- data-ready, actively streaming
271:       2  msmgpio 119 Edge  syncboss0    <- wake/control
```

**Implication:** the controller radio firmware is a *host-supplied blob pushed over a service
interface the kernel driver already implements* — not fused into an opaque part. Reflashable
in principle. This makes controller support a tractable hardware-porting task.

**GPLv2 lever:** `CONFIG_OCULUS_SWD_SYNCBOSS` is a kernel driver, so Meta is obligated to
publish its source — along with the `monterey` `.dts`, the OLED panel driver, and the VS1
board driver. If that release exists, we get the device tree *as source* and skip DTB
decompilation entirely. **Highest-leverage next lookup.**

## Blob inventory (all in `/system/lib64`, all pulled)

| Blob | Size | Function | Open replacement |
|---|---|---|---|
| `libtrackingengines.so` | **25 MB** | Insight SLAM | Basalt / OpenVINS in Monado |
| `libossdk.oculus.so` | 1.5 MB | OS SDK | Monado |
| `vendor.oculus.hardware.sensors@1.0.so` | 1.2 MB | sensor HAL | mainline IIO |
| `libvrsensors-hidlwrapper.so` | 477 K | sensor HIDL | — |
| `libvrapi.so` | 268 K | VrApi runtime | Monado |
| `libopenxr_forwardloader.oculus.so` | 268 K | OpenXR loader | Monado OpenXR |
| `libtrackingutils.so` | 188 K | tracking support | — |
| `libtrackinginjection-service.so` | 158 K | tracking injection | — |

Vendor HAL services seen by name (unreadable): `vendor.oculus.hardware.devicecert@1.0`
(**attestation** — check what it gates before removal), `...sensors@1.0-service` + `-iad`,
`...catty@1.0-service`, `...clocks@1.0`, `...telemetry@1.0`,
`android.hardware.graphics.composer@2.1-service.monterey`.

## Strategic conclusion: do not analyze the SLAM algorithm

`libtrackingengines.so` is **stripped**, exports only **303 dynamic symbols across 25 MB**, and
NEEDs only `liblog/libdl/libm/libc` — i.e. all math/CV/SLAM is statically linked in and
stripped. Direct algorithm analysis would be a multi-thousand-hour dead end.

**Treat it as a replaceable black box.** Basalt/OpenVINS does not need Meta's
algorithm. It needs the three inputs Meta's blob consumes:

1. Raw frames + hardware timestamps from the 4 tracking cameras
2. IMU samples at rate, time-synced to those frames (SyncBoss's job)
3. Per-unit calibration (intrinsics/extrinsics) — likely the `vision` partition

=> **Analysis target is the interfaces + calibration format, not the algorithm.**

## Target stack

| Layer | Meta today | Open replacement | Confidence |
|---|---|---|---|
| Kernel | msm-4.4 downstream | mainline / pmOS `msm8998` | high |
| GPU | KGSL + Adreno blob | Mesa **Freedreno** (a540 mature) | high |
| Display | Meta panel + timewarp | DRM/KMS panel + Monado reprojection | medium |
| Runtime | VrApi / Meta OpenXR | **Monado** | high |
| Tracking | `libtrackingengines.so` | Basalt / OpenVINS | medium |
| Controllers | proprietary 2.4 GHz via SyncBoss | RF/protocol analysis + SWD firmware path | low-medium |

### Irreducible nonfree floor
Cannot be removed on this silicon: Qualcomm **PBL** (mask ROM), **XBL**, Adreno **zap shader**
(must be signed), **Hexagon DSP** firmware, **WCN3990** Wi-Fi firmware.
=> Achievable end state is **zero Meta blobs**, matching the nonfree footprint of any
mainline-Linux Qualcomm phone. Not "zero blobs".

## Temporary owner-admin access status

Temporary owner-admin ADB access is proven on this exact stock build. The 2026-08-30
QuestStack local privilege step (`ionstack`) selected `quest1_monterey_msm8998` for
incremental `49845030443200410`, got `adbd` to `uid=0`, set SELinux permissive for the local
maintenance session, and kept the elevated ADB session usable until reboot. After the final
reboot/wipe, expect the standard Android shell unless that local maintenance step is rerun or
a prepared admin-capable boot image is booted.

Administrator access still gates: `/vendor` contents, SyncBoss live protocol capture, and
**all partition dumps** (incl. `vision` calibration and `boot_a`/DTB). The chicken-and-egg is
gone: the verified local maintenance step can be rerun to dump `boot_a`, then an
admin-capable boot image can be produced from the device's own active-slot boot image if
desired.

## Owner unlock and serviceability record

QuestStack source is on this machine at `/home/ryanm/diytech/QuestStack`. Confirmed flow
(`QuestWorkflow.cs`, `Firmware.cs`) and 2026-08-30 run:
1. Started on slot `_a`, build `50.0.0.198.257.455910822`
   (`49845030443200410`), with device initially reporting locked in the v29 service
   bootloader.
2. Used the verified local maintenance step on the current stock Android build to get a
   temporary owner-admin ADB shell.
3. Backed up the original inactive slot `_b` bootchain before replacement.
4. Wrote a complete unlock-capable v29 bootchain to `_b`:
   `boot, modem, pmic, rpm, tz, hyp, devcfg, cmnlib, cmnlib64, keymaster, ovrtz, abl, xbl`.
5. Switched active slot to `_b`, rebooted into the unlock-capable v29 fastboot/ABL
   (`16476800119700000`), and completed owner-authorized bootloader unlock.
6. Confirmed `getvar:unlocked = yes` and `Device critical unlocked = true`; reset rollback
   indexes to `1`.
7. Switched the active-slot pointer back to `_a`, erased `userdata` and `misc`, and rebooted.

=> **Live Slot B is the access-preservation slot.** It currently holds the unlock-capable
v29 bootchain, especially `abl_b`, used as the bootloader-unlock / service-recovery
mechanism. Do **not** reuse slot B until this live unlock-capable bootchain has been dumped
and verified. Its value is the *image*, not the slot.

Expected live unlock-capable slot B hashes after v29 replacement:

> **Hash-type caveat.** The values in this table are QuestStack *image-length*
> write-verify hashes (they cover only the exact firmware image bytes written into each
> partition). They intentionally **do not** match the *full-partition* dumps in the
> owner-admin backup, which include trailing padding — e.g. this table's
> `boot_b = a1bf0f8a…` vs the full-partition dump `boot_b = 847d7eac…` in
> `backups/1PASH9ACHD0215-2026-08-30T15-20-root/SHA256SUMS`. The same distinction applies
> to the reference stock `boot.img` hash below vs the 64 MiB `boot_a`/`boot_b` partition
> dumps. Do not treat a difference between an image-length hash and a full-partition hash as
> a verification failure. Full-partition dumps were independently verified against the live
> device (`recon/device-sha256-root.txt`) during the owner-admin session.

| Partition | SHA-256 |
|---|---|
| `xbl_b` | `6ed2c6854f4837710b4b3ba31da33820e80841c7f3e69678a680d821efabca63` |
| `abl_b` | `c2cc1f173bec2956fa5e068abc98db82e1cd2c651a4f4bb7ae31c79605e43707` |
| `rpm_b` | `85c4d8ed165a9295a46f20c9c8bd485c42b3b7cd60e832c9c0da0b933dcc5b23` |
| `tz_b` | `a1548108ad612dde59f69ed85cef99a312c761dd3b2eecce3df6602e96d28a10` |
| `hyp_b` | `e14251a9e7932c1ffd2c3c90c000382144a94ce51712177a24c853c58e3055cc` |
| `devcfg_b` | `69a7d7a6656b077b511910e6efaa31e771e7ed24ba80f0c1c694fb164f16575a` |
| `pmic_b` | `57daeeb2903e01bbe932daa8d15129dcf210275b200f696ed2f4c612a4698bf5` |
| `cmnlib_b` | `dd186828e790f7dd547f002ad0fb46f76037e080bcd4e70c7cdda901f68bd685` |
| `cmnlib64_b` | `8e62871f97bf99be78a92ba06b2f51c705c57c50df1ddec40e995b135f63a6e1` |
| `keymaster_b` | `ebaa111a767e1e393133b9c88e54efec32aa143e1fffb77297dcc3a317c22f3c` |
| `ovrtz_b` | `41afd6068117dad22315bebc478f8749f2f505d25a6c27ea7ace4a152f054f9b` |
| `modem_b` | `d8e94155770106d0a615b25e60dec63b4b17384b442f51bd57e561f3a7ace25d` |
| `boot_b` | `a1bf0f8abd84779f7eb8b26794209fa96290f66c5232609999a4d01af39f986d` |

### Pre-unlock Slot B backup exists
`1PASH9ACHD0215-2026-08-30T11-37-13-847Z.zip` is the verified backup taken **before** the
Quest 1 bootloader was unlocked / before QuestStack replaced inactive slot B with the
unlock-capable bootchain. Manifest: `mode=unlock`, `targetSlot=_b`, `complete=true`, includes
`xbl_b`, `abl_b`, `rpm_b`, `tz_b`, `hyp_b`, `devcfg_b`, `pmic_b`, `cmnlib_b`, `cmnlib64_b`,
`keymaster_b`, `ovrtz_b`, `modem_b`, and `boot_b`.

This preserves the original pre-unlock inactive-slot bootchain, **not** the current
unlock-capable ABL / service-recovery image now expected on slot B. It also does **not**
include `vision`, `persist`, `private`, active slot A, `system`, or a full raw disk image. =>
it is not a full device backup.

### Owner-admin backup now exists
`backups/1PASH9ACHD0215-2026-08-30T15-20-root/` was captured from the live device during a
temporary owner-admin ADB session. It includes verified full copies of:

- `vision` — per-unit calibration candidate, 512 MiB
- `persist` and `private`
- active `boot_a`
- `system_a` and `system_b`
- `userdata` as compressed `userdata.img.gz` from a live post-wipe Android session
- every remaining by-name partition as a raw `.img`
- live unlock-capable slot-B service chain: `xbl_b`, `abl_b`, `rpm_b`, `tz_b`, `hyp_b`,
  `devcfg_b`, `pmic_b`, `cmnlib_b`, `cmnlib64_b`, `keymaster_b`, `ovrtz_b`, `modem_b`,
  `boot_b`
- live FDT as `fdt.dtb`
- `/sys/firmware/devicetree/base` as `devicetree.tar`
- first/last 1 MiB GPT/header captures for raw UFS devices `sda` through `sdf`

Hashes and sizes are recorded in
`backups/1PASH9ACHD0215-2026-08-30T15-20-root/MANIFEST.md`. This removes the immediate
`vision`/slot-B data-loss risk and fills the earlier `system_a`/`system_b`/`userdata` gap.
Remaining backup gap: a monolithic full raw disk image and an offline-stable `userdata`
image, if needed. The current `userdata` copy is crash-consistent from a mounted post-wipe
Android session, not an offline-stable filesystem image.

Permission-access details for this session are recorded in
`notes/02-owner-admin-adb-access.md`.

### Firmware sources
- **Clean stock OTA** (extract `boot.img` from this):
  `https://files.cocaine.trade/firmware/meta/Quest/q1_49845030443200410.zip`
  SHA-256 `7c1f75ecd807b59d5215a67ff69e8e8c6da4b966eb08464d48e00d2d5a8b6aaa` (796 MB).
  Runtime 50.0.0.198.257.455910822, built Wed Jul 31 02:02:22 PDT 2024 (matches on-device
  kernel build timestamp 02:31:55 same day).
- **Reference stock `boot.img` SHA-256** (from QuestStack `Firmware.cs`, verify before patching
  AND against live `boot_a` during an owner-admin session):
  `A1BF0F8ABD84779F7EB8B26794209FA96290F66C5232609999A4D01AF39F986D`
- QuestStack's own v29 unlock bundle (separate, not needed now):
  `https://files.catbox.moe/fcpm6p.zip` SHA-256 `7020F22D7CE788CE944E5CB04E5A89830B9A2F4D5AB11D5D08B01687FDC67501`.

### Reusable dd recipe (QuestStack `Firmware.cs:458-478`)
Proven read/write/verify over `/dev/block/bootdevice/by-name/<part><suffix>`:
- write: `dd if=$img of=$blk bs=1M; sync`
- verify: hash image, then `dd if=$blk bs=4096 count=ceil(n/4096) | head -c n | sha256sum`, compare.
Reuse verbatim for partition dumps.

### Magisk-FreeXR verdict
Real (`github.com/FreeXR/Magisk-FreeXR`, GPLv3) but documents NO Quest 1 support; companion
repo targets Quest 3/3S (eureka/panther). Its Quest adaptations assume their device-specific
unlock path, which is unnecessary here (already unlocked). Treat stock Magisk and
Magisk-FreeXR as equal candidates; RAM-boot-test whichever first.

## Next actions

1. Archive the owner-admin backup off-device/off-machine.
2. Optionally capture a monolithic raw disk image or offline-stable `userdata` image.
3. Locate Meta's GPLv2 kernel source release -> `monterey` `.dts` + syncboss/panel/VS1 drivers.
4. Decompile `boot_a`/`fdt.dtb`; cross-check against GPL `.dts`.
5. Map camera + IMU data path; decode `vision` calibration format.
6. Bring up pmOS/mainline on **slot B** only after confirming the access-preservation images
   are archived off-machine, keeping slot A stock as rollback.
