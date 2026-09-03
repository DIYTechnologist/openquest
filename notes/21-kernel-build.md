# Instrumented kernel: built, flashed, FAILED TO BOOT, recovered — 2026-09-03

> Header updated after the fact. The flash was subsequently authorised, attempted, and failed;
> the device was fully recovered. See "FLASH ATTEMPTED AND FAILED" at the end — and note that the
> risk assessment in the "Flashing" section below was **wrong**, which is left in place rather than
> edited so the error is visible.

Reproduce: `tools/kernel-patches/build.sh [instrument]`.

## The build is faithful to what Meta shipped

| | device | our build |
|---|---|---|
| version | `4.4.205-perf+` | `4.4.205-perf+` |
| compiler | `gcc 4.9.x 20150123 (prerelease)` | `gcc 4.9.x 20150123 (prerelease)` |
| config | `recon/config.gz` (from `/proc/config.gz`) | **0 differing lines** before deliberate changes |
| kernel size | 19 232 110 B | 19 375 577 B baseline (+0.7 %) |

The compiler string matches byte-for-byte because the AOSP `aarch64-linux-android-4.9` prebuilt is
literally the compiler Meta used. Config is the device's own, not a defconfig — `monterey_defconfig`
exists but does **not** enable `CONFIG_SYNCBOSS`, so it is not what shipped.

## Config changes: three lines, all deliberate

```
CONFIG_SYSTEM_TRUSTED_KEYS="verity.x509.pem" -> ""     (required; see CONFIG_DEVIATIONS.md)
# CONFIG_MSMB_CAMERA_DEBUG is not set -> =y            (instrumentation)
# CONFIG_DYNAMIC_DEBUG is not set     -> =y            (instrumentation)
```

`SYSTEM_TRUSTED_KEYS` is forced: Meta's GPL release omits `verity.x509.pem`. It is safe because the
keyring has **no consumer** in this config — `MODULE_SIG` off, zero modules loaded,
`DM_VERITY_AVB` off (Android dm-verity takes its key from the fstab table). An empty keyring is
also more honest than injecting our own key under Meta's name.

## Why instrumentation needs a rebuild at all — and what it corrects

I checked for a runtime path first. There isn't one: **`CONFIG_DYNAMIC_DEBUG` is not set** on the
device, so every `pr_debug` compiles to nothing and `/sys/kernel/debug/dynamic_debug` does not
exist. Confirmed on-device.

Reading the driver to place instrumentation turned up something that **invalidates an earlier
conclusion**. In `msm_csid.c`, with `CONFIG_MSMB_CAMERA_DEBUG` unset:

```c
#else
static void msm_csid_set_debug_reg(struct csid_device *csid_dev,
	struct msm_camera_csid_params *csid_params) {}      /* empty */
#endif
```

`msm_csid_set_debug_reg()` is the function that **programs the CSID interrupt mask**. On the stock
kernel it is an empty stub, so CSID raises only reset-done (`0x800`) and never any data or error
interrupt — **for the vendor's working path too**.

`notes/19` repeatedly treated "CSID shows only `0x800`" as evidence that no CSI data was arriving.
That inference was wrong. The stock kernel cannot report data interrupts at all. The instrumented
build makes that function real, so for the first time CSID interrupt status becomes meaningful
evidence either way.

This is also why the instrumentation is **config-only** — no driver logic is patched. Qualcomm's own
debug switch does exactly what hand-added `printk`s would have, and turning it on is far lower risk
than editing driver code.

## Build fixes required (`0001-build-fixes-for-modern-host.patch`, 34 lines)

Meta's GPL release does not build as-shipped on a 2026 host:

1. **`drivers/staging/oculus/internal/` is referenced but not shipped** — `Kconfig` sources it and
   `Makefile` has `obj-y += internal/`. Removed from the Makefile rather than stubbed with a fake
   object: an empty `built-in.o` would silently imply the code is present. No `CONFIG_` symbol from
   that directory is enabled in the device's config, so nothing is lost. (A real GPL-compliance gap
   on Meta's side.)
2. **`scripts/Makefile.lib` passes `-Wno-` flags for dtc checks the in-tree dtc 1.4.2 lacks**, which
   makes dtc hard-error with "Unrecognized check name". Meta evidently built with a newer external
   dtc. Dropping them is a no-op for the generated DTBs — each only *disables* a check this dtc
   never runs.
3. Host-side only, no patch needed: `HOSTCFLAGS` needs `-fcommon` (GCC 10+ changed the default,
   breaking `scripts/dtc`'s `yylloc`), and `-I` for Homebrew's openssl because Fedora's OpenSSL 3.x
   no longer ships `openssl/engine.h` that `scripts/extract-cert.c` includes.

## Flashing: NOT done, and here is the decision

Artifacts: `work/kbuild/out/Image.gz-dtb.instrumented` (20 539 576 B).

**The real risk is root, not bricking.** *(This turned out to be wrong — the actual outcome was
a non-booting device. Left unedited; see the post-mortem at the end.)*

- Bootloader is **unlocked** (`ro.boot.flash.locked=0`), slot `_a`, so a bad boot image is
  recoverable by flashing `backups/boot-monterey/stock_boot.img` back via fastboot.
- **Root comes from Magisk 30.7 patching the boot image.** A plain repack with our kernel would
  produce an *unrooted* boot image — and every piece of camera tooling here needs root. The new
  kernel must be Magisk-patched, or root is lost until it is.
- Boot image is a standard `ANDROID!` v0 image, 4096-byte pages: kernel 19.23 MB + ramdisk
  10.61 MB into a 64 MB partition. Ours is 20.5 MB, so 31.1 MB total — fits comfortably.
- `/system` dm-verity (`veritymode=enforcing`) is unaffected: we would replace only `boot`.

**Points to settle before flashing:**

1. Repack with the **existing ramdisk** from `stock_boot.img`, then Magisk-patch, so root survives.
2. Flash to the **inactive slot** (`_b`) if we want a one-reboot fallback, or accept
   fastboot-restore-from-backup as the recovery path on `_a`.
3. Confirm the device can be put into fastboot reliably before we need it, not after.

Nothing here is urgent — B1 still works and no other workstream is blocked on it.

## FLASH ATTEMPTED AND FAILED — device recovered, 2026-09-03

The instrumented kernel was flashed to `boot_a` and **did not boot**. The device was recovered to
its exact prior state. Recording this properly because the failure mode is the useful part.

### What happened

1. Repacked `stock_boot.img` with our instrumented kernel (stock ramdisk, stock cmdline, stock
   `kernel_dtb` — only the kernel binary changed), then Magisk-patched it with the device's own
   `/data/adb/magisk/boot_patch.sh`.
2. Verified before flashing: kernel inside the image was **md5-identical** to our build
   (`52335e7f…`), `magiskboot cpio test` returned 1 (Magisk-patched), image backed up to the host,
   and `fastboot devices` confirmed working.
3. `fastboot flash boot_a` → OKAY. Reboot → **no USB enumeration at all**, in any mode.
4. On-screen: `device unlocked → device corrupt → Meta logo → device unlocked`, cycling.
5. Restored `new-boot_magisk30.7.img` (md5 `e534bd75…`). It did **not** come straight back — same
   cycle persisted for several minutes.
6. It recovered on its own shortly after. Final state verified: `boot_completed=1`, slot `_a`,
   stock Meta kernel, Magisk 30.7 root, `trackingservice` + sensors HAL running, SELinux
   Enforcing, and **all 4 cameras capturing** via B1.

**Nothing was lost.** Only `boot_a` was ever written; `/system`, `/data`, `vbmeta` and the
bootloader partitions (`abl`/`xbl`) were never touched.

### Why it failed — not yet established

Honest answer: unknown. Candidates, none confirmed:

- **AVB rejection of the boot image.** The `device is corrupt` screen is AVB reporting the boot
  image is not signed with Meta's key — but that is equally true of the Magisk image the device
  runs happily every day, so on its own this does not explain a failure to boot.
- **A/B slot fallback.** This is an A/B device. Bootloaders mark a slot unbootable after repeated
  failed boots and fall back to the other slot. That would explain the most confusing observation —
  why restoring a known-good image to `boot_a` did not immediately fix it — because the bootloader
  may have stopped using `boot_a`. Not verified: the device recovered before slot state could be
  read from fastboot.
- **Kernel size / load address.** Our instrumented kernel is ~1.1 MB larger than stock
  (`CONFIG_DYNAMIC_DEBUG` tables + `CONFIG_MSMB_CAMERA_DEBUG` strings). It fits the 64 MB partition
  easily, but a decompressed-image or `tags_addr` constraint is not something we checked.

### The methodology error

I flashed a boot image that changed **two things at once**: the repack/Magisk-patch chain, and the
kernel binary. When it failed, those could not be separated.

The correct first step was to repack `stock_boot.img` with the **stock kernel** — a no-op
round-trip through `magiskboot` + `boot_patch.sh` — and flash that. If it boots, the toolchain is
proven and any later failure is attributable to our kernel. If it does not boot, the problem is the
repack/AVB path and our kernel was never implicated at all.

I named AVB rejection as a risk beforehand and then under-weighted it, telling the user the main
risk was "losing root, not bricking". That framing was wrong: the actual failure was a
non-booting device, and it cost a recovery cycle.

### Where this leaves things

- **B1 works; nothing is blocked.** All stock-OS workstreams are unaffected.
- The instrumented kernel still builds reproducibly (`tools/kernel-patches/build.sh instrument`)
  and the verified image is kept at `backups/boot-monterey/new-boot_instrumented-kernel.img`
  (md5 `aa54d611…`) — do **not** reflash it without doing the stock-kernel round-trip test first.
- **Next attempt, in order:** (1) stock-kernel repack round-trip to prove the chain; (2) if that
  boots, read `slot-unbootable`/`slot-retry-count` from fastboot *before and after*; (3) only then
  reintroduce the instrumented kernel; (4) have the device on a charger and expect to sit in
  fastboot between attempts.
