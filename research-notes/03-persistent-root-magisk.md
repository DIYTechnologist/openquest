# Quest 1 Persistent Owner-Admin — Magisk-Patched Boot

Date: 2026-08-30
Device serial: `1PASH9ACHD0215`
Device: Quest 1 / `monterey`, slot `_a`, build `49845030443200410`
Status: **COMPLETE — `magisk_legacysar_boot_a.img` flashed to `boot_a`; persistent Magisk root verified across a full reboot; `su` grant for shell is Forever and survives reboot.**

## Goal

Replace the temporary `ionstack` root (cleared every reboot) with durable, reversible root
that survives reboots, via a Magisk-patched `boot_a`. Chosen over the minimal
`androidboot.adb.rootable=1` cmdline hook because the porting/backup work needs on-demand
SELinux permissive + `su`, not just enforcing `adb root`.

## Why this is low-risk / reversible

- Bootloader UNLOCKED (`verifiedbootstate=orange`): device boots unsigned/AVB-invalid boot.
- Only `boot_a` is modified. `boot` is not the bootloader; a bad boot image = bootloop,
  recoverable via `fastboot flash boot_a <stock>`. `xbl`/`abl`/`tz`/etc. are never touched.
- Stock `boot_a` is preserved read-only in the gold backup **and** re-downloadable from the
  stock OTA, so the restore image is doubly safe.
- Slot B (unlock-capable v29 chain) is NOT touched.

## Source tool

Magisk v30.7 (official release APK), GPLv3.

| Field | Value |
|---|---|
| APK | `Magisk-v30.7.apk` |
| APK SHA-256 | `e0d32d2123532860f97123d927b1bb86c4e08e6fd8a48bfc6b5bee0afae9ebd5` |
| Patch method | on-device `boot_patch.sh` (arm64 magiskboot) in `/data/local/tmp/bootpatch` |
| Root needed to patch? | No — file ops only, ran as `shell` uid=2000 |

## Boot image facts (from `magiskboot`/`unpack_bootimg`)

- Header **version 0**; pagesize 4096; system-as-root (`init -> /system/bin/init`).
- DTB is **appended inside the kernel blob** (`KERNEL_DTB_SZ 5306210`), second_size=0.
  => kernel blob must be preserved verbatim; Magisk does this (no Samsung/LEGACYSAR patch).
- AVB1-signed input; signature dropped on repack (OK on orange).

## Patch flags used (SUPERSEDED — `LEGACYSAR` was wrong)

> **Correction 2026-08-30 22:3x:** `LEGACYSAR=false` was incorrect and is the sole cause of
> the two failed RAM-boot attempts. See "Root Cause: Legacy SAR" below. Correct flags are
> identical except `LEGACYSAR=true`.

```
KEEPVERITY=true        # system dm-verity stays intact (we don't modify system)
KEEPFORCEENCRYPT=true  # leave userdata FBE alone
PATCHVBMETAFLAG=false  # no vbmeta partition
RECOVERYMODE=false     # patching boot, not recovery
LEGACYSAR=false        # WRONG -- see correction above; must be true
```

The 3x "Failed to patch" on `kernel_dtb` is the intended no-op: KEEPVERITY=true means there
is nothing to strip from the DTB fstab.

## Verification (host-side, unpack + compare)

| Component | Result |
|---|---|
| kernel blob | **IDENTICAL** `8865a88e9747f60be9a7e2568417d73b8d1cd02a1689156a84312471c079caee` (appended DTB preserved) |
| ramdisk | changed as expected (10610448 -> 10909672 B, magiskinit + overlay.d) |
| header | v0 / pagesize 4096 / cmdline preserved |

Artifacts (working dir `work/boot-root/`):

| File | SHA-256 |
|---|---|
| stock `boot_a.img` (== gold) | `7ec81a30c8f7b597dd2678b031a136f2690db5bb3f18996b23c31544e2917c9e` |
| `magisk_patched_boot_a.img` | `2d1d3b8d4610ef810033062d1d164adf76385fcd88eaa85edb70055117ec88c5` |

## RAM-Boot Test Results

Fastboot RAM boot is confirmed supported on this ABL. A marker boot image with an
`androidboot.magisktest=hello42` cmdline property booted and exposed the marker through
Android properties, proving `fastboot boot <image>` really uses the supplied boot image.

Magisk-patched images tested so far:

| Image | Result |
|---|---|
| `magisk_patched_boot_a.img` / Magisk v30.7 | Android boots cleanly, but Magisk does not activate |
| `magisk_v27_patched_boot_a.img` / Magisk v27.0 | Android boots cleanly, but Magisk does not activate |

Observed behavior after RAM boot:

- Android reaches `boot_completed=1`; no boot loop.
- Active slot remains `_a`; `boot_a` on flash is untouched.
- `su`, `/data/adb`, and Magisk daemon are absent.
- SELinux remains enforcing unless temporary owner-admin ADB access is re-established.

Interpretation: the RAM-boot path is valid, but Magisk's early init path is falling back to
normal boot on this Oculus Android 10 boot layout. This is not yet safe to flash as durable
owner-admin access, because the patched boot image currently behaves like stock Android.

## Current State Before Log Inspection

As of the next diagnostic step:

| Check | Result |
|---|---|
| ADB authorization | connected and authorized |
| Active slot | `_a` |
| Build incremental | `49845030443200410` |
| ADB shell | `uid=2000(shell)` |
| SELinux | `Enforcing` |
| `ro.boot.magisktest` | blank |
| Staged `ionstack` | `/data/local/tmp/q1u/ionstack`, SHA-256 `d65c800a2a032d3af579b8c9e5e11c12ae9b344d95c545396f1c83ccc6d4fc47` |

## Intended Next Diagnostic Step

Re-establish temporary owner-admin ADB access with the already verified local maintenance
binary, then capture logs from the current boot:

- `dmesg` / kernel ring buffer, looking for `magisk`, `magiskinit`, `init`, ramdisk, SAR,
  overlay, mount, and SELinux messages.
- `logcat -b all`, looking for Magisk app/daemon attempts or init/service failures.
- Filesystem checks for `/data/adb`, `/sbin`, `/debug_ramdisk`, `/overlay.d`, and Magisk
  staging artifacts.

Goal: determine whether Magisk is not executing at all, executing but rejecting this boot
layout, losing its overlay/stub paths, or being blocked later by init/SELinux policy.

## Diagnostic Session: 2026-08-30 22:21-22:24

Temporary owner-admin ADB access was re-established with the verified staged `ionstack`
binary at `/data/local/tmp/q1u/ionstack`.

Observed success markers:

```text
uid=0(root) ... context=u:r:shell:s0
Permissive
ROOT VERDICT root=1 uid=2000->0 selinux_permissive=1 (1->0) persist=1(adbd)
xrw: ROOT held -- box staying up, adbd is root, `adb shell` gives root. Not tearing down.
```

The following captures were saved before the device reset:

```text
recon/magisk-ramboot-2026-08-30/dmesg.txt
recon/magisk-ramboot-2026-08-30/logcat-all.txt
recon/magisk-ramboot-2026-08-30/proc-cmdline.txt
recon/magisk-ramboot-2026-08-30/getprop.txt
recon/magisk-ramboot-2026-08-30/mount.txt
recon/magisk-ramboot-2026-08-30/ps-A.txt
recon/magisk-ramboot-2026-08-30/ionstack-magiskdiag.log
```

Initial read:

- No `magisk` or `magiskinit` strings were found in `dmesg` or `logcat`.
- Kernel command line still includes `skip_initramfs rootwait ro init=/init`.
- Android init logs show stock first stage and second stage init:
  `init first stage started!`, then `init second stage started!`.
- Init loads `/debug_ramdisk/adb_debug.prop`, confirming the debug ramdisk path exists in
  this boot flow.
- `ro.boot.verifiedbootstate=orange`, `ro.boot.flash.locked=0`, and active slot `_a`.

While retrying the filesystem path probe with portable `ls`/`find` syntax, the ADB link
closed and the headset reset. After it returned:

```text
uid=2000(shell)
Enforcing
ro.boot.slot_suffix=_a
ro.boot.verifiedbootstate=orange
sys.boot_completed=1
ro.boot.magisktest=<blank>
ro.build.version.incremental=49845030443200410
```

Interpretation: the temporary owner-admin state was cleared by the reset, as expected. No
flash operation was performed; `boot_a` on storage remains stock. The useful next step is
offline review of the captured logs and boot-image layout, not another flash attempt.

## Root Cause: Legacy SAR (2026-08-30, temp-root session)

The two failed images were patched with `LEGACYSAR=false`. That was wrong. This device is
**legacy system-as-root** (Android-9 style), confirmed four independent ways:

| Evidence | Observation |
|---|---|
| Live kernel cmdline | `skip_initramfs rootwait ro init=/init`, `root=/dev/dm-0`, `dm="system none ro,0 1 android-verity /dev/sda6"` |
| Root mount | `/dev/root / ext4 ro,seclabel,nodev,relatime` — **no `rootfs`/`tmpfs` mount exists at all** |
| Stock boot ramdisk | `init` is a *symlink* to `/system/bin/init`; there is **no init ELF in the ramdisk**. A real 2SI device ships an actual init binary here. |
| `fstab.monterey` | `by-name/system  /  ext4 ro,barrier=1  wait,slotselect,verify` — system mounts at `/`; no `/metadata`, no dynamic partitions |

Because the bootloader passes `skip_initramfs`, the kernel discards the boot ramdisk entirely
and runs `/init` from the dm-verity'd system partition. `magiskinit` therefore never executes,
no matter how the ramdisk is patched — which is exactly why zero `magisk`/`magiskinit` strings
appeared in `dmesg` or `logcat`.

Magisk's **own autodetection would have set this correctly**. From `util_functions.sh:320`:

```sh
grep ' / ' /proc/mounts | grep -q '/dev/root' && LEGACYSAR=true
```

The device's `/proc/mounts` matches that rule exactly. The manual override to `false`
overrode a correct autodetect.

The earlier note "system-as-root (`init -> /system/bin/init`)" was read off the ramdisk and
misinterpreted as evidence of a modern 2SI device. It is the opposite: a ramdisk whose `init`
symlinks into `/system` is the signature of legacy SAR.

### Corrected build

Re-patched on-device with `LEGACYSAR=true` (Magisk v30.7, all other flags unchanged):

```
Patch @ 0x01DF95B8 [736B69705F696E697472616D667300] -> [77616E745F696E697472616D667300]
```

`0x01DF95B8` = 31430072, which matches byte-for-byte the single `skip_initramfs` occurrence
located independently in the decompressed stock kernel.

| Check | Result |
|---|---|
| `want_initramfs` in patched kernel | 1 occurrence |
| `skip_initramfs` in patched kernel | 0 occurrences |
| `kernel_dtb` size | `5306210` — **unchanged**, appended DTB preserved |
| ramdisk `init` | now real `magiskinit` + `overlay.d` (was a symlink) |

| File | SHA-256 |
|---|---|
| `work/boot-root/magisk_legacysar_boot_a.img` | `e3ed6a3d3a95d010cf02c8f7c3311084012440b6fc271fad29de648eca6777f4` |

Note: the earlier verification criterion "kernel blob **IDENTICAL**" is no longer the pass
condition. A correct legacy-SAR patch *must* change the kernel. The DTB-preservation check
(`kernel_dtb` size/hash unchanged) is the criterion that still applies.

### Version note

Both Magisk v27.0 and v30.7 implement the legacy-SAR hexpatch (`boot_patch.sh:235` and `:245`
respectively). The Magisk version was never the problem; the flag was. v30.7 is used.

### Corrected failure markers

`/data/adb` **exists on stock** on this device (empty, context `u:object_r:adb_data_file:s0`).
It is *not* a Magisk indicator, contrary to the earlier "`/data/adb` absent" marker. Use
`/data/adb/magisk` and `/overlay.d` instead — both confirmed absent on stock.

### Device gotcha: toybox `grep`

The Quest's on-device toybox `grep` does **not** support `\|` alternation in BRE. It returns
0 matches and exit 1 rather than erroring — a silent false negative that briefly produced the
wrong conclusion that v30.7 lacked legacy-SAR support. Always use `grep -E` on-device.

### Stock baseline captured

Full stock-boot baseline (with temp root) saved for post-RAM-boot diffing:

```text
recon/stock-baseline-2026-08-30/
  fs-paths.txt  mounts.txt  proc-cmdline.txt  getprop.txt
  ps-A.txt  dmesg.txt  fstab.monterey  dirs.txt  selinux-boot.txt
```

This completes the `fs-checks.txt` / `fs-paths.txt` probes that came back 0 bytes when the
ADB link dropped in the previous session. `find` was avoided this time (it was implicated in
that drop); an explicit path list with `ls -ldZ` was used instead.

## RAM-Boot Test of Corrected Image: PASS (2026-08-30)

`fastboot boot work/boot-root/magisk_legacysar_boot_a.img` — nothing written to flash.

```text
Sending 'boot.img' (65536 KB)   OKAY [ 1.989s]
Booting                         OKAY [ 3.978s]
```

| Check | Result |
|---|---|
| Boot | clean, `sys.boot_completed=1`, no bootloop |
| Magisk tmpfs | `magisk /sbin tmpfs rw,seclabel,...` **mounted** |
| | `magisk /sbin/.magisk/worker tmpfs rw,...` |
| `/sbin` contents | `magisk magiskinit resetprop su supolicy` (was: `slideshow sysimgcheck`) |
| `/sbin/su` | symlink -> `./magisk` |
| `magiskd` | running, `root 678 1 ... S magiskd` |
| `magisk -v` / `-V` | `30.7:MAGISK:R` / `30700` |
| SELinux | `Enforcing` (normal; the earlier `Permissive` was ionstack's temp state) |
| Active slot | `_a`, `boot_a` on flash still stock |

This confirms the `LEGACYSAR=true` hexpatch is the complete fix: the kernel now honors the
boot ramdisk, `magiskinit` executes as `/init`, and it hands off correctly to the real
`/system/bin/init` on the dm-verity'd system partition.

### Remaining gap: `su` is denied

`su -c id` fails (exit 255). This is **not** a boot-path problem — `magiskd` is alive and
answering version queries. There is simply no Magisk manager app installed, so magiskd has no
policy database and denies su by default:

```text
/sbin/magisk --sqlite "select * from policies"  ->  Root is required for this operation
```

Resolving this needs the Magisk app installed and an su grant for uid 2000 (shell). The
policy DB lives on `/data`, so it persists across the reboot back to stock and on to a flash.

## FLASHED & VERIFIED (2026-08-30) — persistent root achieved

Owner approved flashing after RAM-boot proved the boot path. Sequence:

1. Pre-flight hashes reconfirmed:
   - to-flash `work/boot-root/magisk_legacysar_boot_a.img` = `e3ed6a3d…` (matches)
   - restore `backups/…-root/boot_a.img` = `7ec81a30…` (matches, read-only)
2. `fastboot flash boot_a work/boot-root/magisk_legacysar_boot_a.img` — OKAY.
3. `fastboot reboot` → **real power-cycle**, not a RAM boot.

Post-flash verification (device came up on its own with Magisk active):

| Check | Result |
|---|---|
| `/sbin` tmpfs | `magisk /sbin tmpfs rw,...` mounted |
| `/sbin` contents | `magisk magisk32 magiskinit magiskpolicy resetprop su supolicy` |
| `magiskd` | running (pid 679) |
| `magisk -V` | `30700` |
| slot | `_a` |

### su grant — validated end-to-end

Magisk app (`com.topjohnwu.magisk`) was already installed on `/data` and survived the flash.
`su -c id` on the stable flashed system:

```text
uid=0(root) gid=0(root) groups=0(root) context=u:r:magisk:s0
Enforcing
```

Genuine Magisk-mediated root with **SELinux still Enforcing** (context `u:r:magisk:s0`), i.e.
not the old permissive ionstack path. Policy persisted:

```text
magisk --sqlite "select * from policies"
  uid=2000|policy=2|until=0|logging=1|notification=1     # policy=2 allow, until=0 Forever
  /data/adb/magisk.db
```

### Reboot-survival test — PASS

`adb reboot`; after `boot_completed`, `su -c id` **without any re-grant** returned
`uid=0(root)` context `u:r:magisk:s0` instantly. Durable owner-admin root now survives
reboots.

### Notes on the su-grant attempts

The two failed grant attempts happened during the **RAM-boot** phase, not because of any boot
or policy problem:

- Magisk's `SuRequestActivity` has a built-in auto-response countdown. On the volatile RAM
  boot the countdown expired before the headset tap registered (observed exit 255 = auto-deny;
  exit 124 = full wait with no answer).
- One RAM-boot session also reset back to stock mid-prompt (RAM boots don't persist), wiping
  the request entirely (`su: not found`, exit 127, uptime ~150s = fresh stock boot).

On the flashed, stable system the Grant tap registered immediately (exit 0). Lesson: do
interactive Magisk grants on a flashed install, not a RAM boot.

### Restore (unchanged, still valid)

```text
fastboot flash boot_a backups/1PASH9ACHD0215-2026-08-30T15-20-root/boot_a.img
```

Post-flash captures: `recon/legacysar-ramboot-2026-08-30/` (magisk-active) and
`recon/stock-baseline-2026-08-30/` (stock baseline for diffing).

## Historical: pre-flash gating (superseded)

Do not flash either Magisk-patched image to `boot_a` until RAM-booted Magisk activation is
understood and verified.

Restore remains:

```text
fastboot flash boot_a backups/1PASH9ACHD0215-2026-08-30T15-20-root/boot_a.img
```
