# Instrumented kernel: BUILT, not flashed — 2026-09-03

Authorised to build only. **Nothing has been flashed and the device is untouched.** This note
records the build and lays out the flashing decision for discussion.

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

**The real risk is root, not bricking.**

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
