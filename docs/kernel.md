# `components/kernel`

Builds the monterey (Quest 1, msm8998) kernel from Meta's published GPLv2 source, optionally with
camera-debug instrumentation. Not a userspace service, but a real replacement binary feeding both
the current camera bring-up and the eventual OS swap (`research-notes/18` step 5).

## Status

Builds and boots. Compiler string, kernel version, and config are byte-identical to the device's
own except three deliberate, documented deviations (`CONFIG_DEVIATIONS.md`, `research-notes/21`).
Currently the device runs Meta's **stock** kernel (`research-notes/32`) — the instrumented build was
reverted after it became the prime suspect in a UFS wedge, so this component's `instrument` variant
is a diagnostic tool for camera bring-up, not what's flashed day to day. See the standing rule in
`research-notes/52` before rebuilding and flashing this.

## What it does

- `make` — baseline: `recon/config.gz` (the device's own running config, not a defconfig — the
  in-tree `monterey_defconfig` is stale and lacks `CONFIG_SYNCBOSS`) with
  `CONFIG_SYSTEM_TRUSTED_KEYS` forced empty (Meta's GPL release omits `verity.x509.pem`; safe
  because nothing in this config consumes the keyring — `CONFIG_DEVIATIONS.md`).
- `make instrument` — adds `CONFIG_MSMB_CAMERA_DEBUG=y` (turns `CDBG` into `pr_debug` across the
  camera stack, and critically makes `msm_csid_set_debug_reg()` a real function instead of an empty
  stub, so the CSID interrupt mask is actually programmed) and `CONFIG_DYNAMIC_DEBUG=y` (the device
  ships with this off, so `pr_debug` compiles to nothing and there is no runtime toggle — a rebuild
  is unavoidable). Config-only; no driver logic is patched.
- The toolchain is AOSP GCC 4.9.x 20150123 (prerelease), which is not an arbitrary choice — it
  produces the exact compiler string in the device's own `/proc/version`, baked into the
  `quest-kernel-toolchain` container image.
- `0001-build-fixes-for-modern-host.patch` fixes three things Meta's GPL release needs on a modern
  host: a referenced-but-not-shipped `drivers/staging/oculus/internal/` (removed from the Makefile,
  a real GPL-compliance gap on Meta's side, nothing lost since no `CONFIG_` symbol from it is
  enabled), unrecognized dtc check flags for the in-tree dtc 1.4.2, and host toolchain flags
  (`-fcommon` for GCC 10+, baked into the container rather than patched).

## Build & run

```
make -C components/kernel              # -> components/kernel/build/out/arch/arm64/boot/Image.gz-dtb
make -C components/kernel instrument
```

Needs `work/oculus-kernel` (Meta's published GPLv2 source) and `recon/config.gz` (the device's own
`/proc/config.gz`) present in the checkout; both are mounted into the build container.

## Known limits

- Flashing an instrumented build carries real risk — one flash attempt failed to boot and required
  a full recovery (`research-notes/21`); always keep a verified `boot_a` backup first
  (`backups/boot-monterey/`).
- This is not yet a mainlined or LineageOS-portable kernel; it is Meta's own tree with minimal,
  documented deviations, which is deliberate for now (`research-notes/18` step 5: "mainlining is not
  a prerequisite, though it is the cleaner end state").
