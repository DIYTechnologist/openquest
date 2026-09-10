# Step 5 — pivot to hand-building, decisive test image ready, needs device to run — 2026-09-09

Continuation of `research-notes/79`. User's call after `research-notes/79` confirmed lineage-17.1
has the identical unconditional-two-stage-init text as lineage-18.1 (no build needed, just grepped
the real synced source — dates to a Sept 2018 AOSP commit, right at the Android 9 boundary): stop
chasing older AOSP branches for a reference build, hand-build the ramdisk directly instead.

## Found the real stock ramdisk artifacts already on disk

`work/boot-root/v_orig/ramdisk` — checked its size (10610448 bytes) against the SHA/size table in
`research-notes/03` ("stock `boot_a.img` (== gold)", ramdisk changed 10610448 -> 10909672 B once
Magisk-patched) — **10610448 is the exact pre-Magisk stock size**, confirming this is genuinely
pristine, not a Magisk-patched capture. Also directly re-verified `work/boot-root/boot_a.img`'s
SHA-256 against research-notes/03's recorded gold hash: exact match.

## Correction to research-notes/74's description of the stock ramdisk

Extracted it fresh (`cpio -id`) rather than trusting the prose description. research-notes/74 said
stock's `init` is "a real init binary (199KB, old-style)" — **checked directly, this is wrong**:
`init` in the actual stock ramdisk is a **symlink to `/system/bin/init`**, not a real binary at all.
Everything else in 74's description holds up: real `init.rc`, `ueventd.rc`, `ueventd.monterey.rc`,
one combined `sepolicy`, `fstab.monterey`, `verity_key`, direct `bin -> /system/bin` / `vendor ->
/system/vendor` / `product -> /system/product` symlinks, all living directly at ramdisk root. Also
present (not mentioned in 74): `adb_debug.prop`/`adb_insecure.prop` (Oculus's own manufacturing/
debug build config, not a Magisk artifact — real stock content), `mfgmode_plat_sepolicy.cil`, empty
`debug_ramdisk`/`sbin` directories (present but unused in this boot flow), a `res/` directory.

This is a genuine hybrid, older than what either lineage-18.1 or lineage-21's build systems produce
by default: init.rc/sepolicy/ueventd/fstab embedded directly (unlike modern two-stage-init's
first-stage-only shape), but init itself deferred to system.img via symlink (unlike a fully
self-contained classic ramdisk that would need its own standalone init binary). Neither AOSP build
config tried so far (SAR true/false on either lineage-18.1 or -21) produces this exact shape --
consistent with the decision to hand-build rather than keep chasing an AOSP branch that happens to
default to it.

## Diagnostic test image built (not yet run — needs the physical device)

Rather than jumping straight to a full custom ramdisk paired with the from-scratch lineage-21
Android 14 `system.img` (a large surface for NEW mismatches -- Android-10-era sepolicy/init.rc
against an Android-14 userspace layout could fail for entirely different reasons even if the
ramdisk shape itself is now correct), built the cheapest possible one-variable test first:

- **Kernel**: our own build (`components/kernel/build/out/arch/arm64/boot/Image.gz-dtb`),
  LEGACYSAR-patched fresh via `patch_legacysar_kernel.py` (same patch already confirmed necessary,
  research-notes/71).
- **Ramdisk**: the exact stock ramdisk file, byte-for-byte, unmodified.
- **Header/cmdline**: every field copied exactly from unpacking the real gold `boot_a.img`
  (`os_version 10.0.0`, `os_patch_level 2024-07`, `pagesize 4096`, `base 0`, `kernel_offset
  0x8000`, `ramdisk_offset 0x1000000`, `tags_offset 0x100`, full real cmdline including
  `androidboot.hardware=monterey`).
- **System partition**: intentionally NOT touched — this is a `fastboot boot` (RAM boot, already
  confirmed supported and non-persistent on this device's ABL, research-notes/03) against whatever
  is already on `system_a` (stock, untouched).

Built at `/tmp/kerneltest/test_ourkernel_stockramdisk.img` (29941760 bytes). Re-unpacked it
afterward and byte-compared both the kernel and ramdisk sections back out against their sources --
both identical, packaging step introduced no corruption.

**This isolates the ramdisk-shape hypothesis as cleanly as possible**: if this boots, it confirms
our kernel build is fine and the from-scratch lineage-21 ramdisk's *shape* (not any content
mismatch with a foreign system.img) was the actual blocker the whole time. If it doesn't boot,
the ramdisk-shape hypothesis from research-notes/74 is wrong or incomplete, and the real cause is
still open regardless of what ramdisk shape gets used.

## Blocked on device access

No device connected as of this note (`adb devices`/`fastboot devices`/`lsusb` all empty). Needs the
headset connected via USB and in fastboot mode to run `fastboot boot
/tmp/kerneltest/test_ourkernel_stockramdisk.img` and observe the result. This is a RAM boot --
doesn't touch the flashed `boot_a`/`system_a` at all, safe to try, but still needs the physical
device present, which this session doesn't have right now.

## Next steps on resume

1. Get the device connected (USB, fastboot mode) -- ask the user if not already done.
2. `fastboot boot /tmp/kerneltest/test_ourkernel_stockramdisk.img`, watch for: reaches a shell /
   `boot_completed=1` (best case -- confirms ramdisk-shape hypothesis outright), reboots to
   bootloader like every from-scratch attempt so far (ramdisk-shape hypothesis likely wrong or
   incomplete), or some new distinct failure mode (informative either way -- check `pstore`
   after, same as research-notes/74's diagnostic discipline).
3. If it boots: the real stock ramdisk content (already sitting in `work/boot-root/v_orig/ramdisk`,
   or freshly re-extracted to `/tmp/stock-ramdisk/extracted/`) becomes the literal template to adapt
   for the lineage-21 `system.img` pairing -- next question becomes how much of the Android-10-era
   init.rc/sepolicy/ueventd content can be reused as-is vs needs regenerating against Android 14's
   actual `/system` layout (SELinux policy version compatibility in particular is a real risk
   flagged above, not yet investigated).
4. If it doesn't boot: re-open the root-cause question -- the ramdisk-shape hypothesis from
   research-notes/74, however well-evidenced structurally, would not be confirmed as the actual
   boot blocker, and pstore/other diagnostics need a fresh look.
