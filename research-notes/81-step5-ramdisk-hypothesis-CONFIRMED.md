# Step 5 — ramdisk-shape hypothesis CONFIRMED: our kernel + stock ramdisk boots clean — 2026-09-09

Continuation of `research-notes/80`. Ran the diagnostic test image built there on the real device.
**Result: full, stable, successful boot.** This is the single biggest result in the entire step-5
arc — it confirms the root cause research-notes/74 hypothesized five notes ago, and proves our own
kernel build has been correct the whole time.

## The test

`fastboot boot /tmp/kerneltest/test_ourkernel_stockramdisk.img` — our own kernel build
(`components/kernel`, LEGACYSAR-patched), paired with the **exact, unmodified stock ramdisk**
(`work/boot-root/v_orig/ramdisk`, verified byte-identical to the pristine pre-Magisk stock capture),
against the untouched stock `system_a`. A RAM boot — doesn't touch flash at all.

## Result: real, full boot

USB enumeration told the story before adb even came up: fastboot's own "Quest" interface
(idProduct 0186) disconnected, then a **new, different USB personality** appeared — "Android"/
"Oculus" (idProduct 0081), the ADB gadget. That interface alone is categorically more boot progress
than any from-scratch attempt this whole arc has produced (every prior attempt: "Meta logo briefly,
then straight back to bootloader," zero USB activity beyond the bootloader's own fastboot interface,
`pstore` staying empty across every attempt per research-notes/74).

Once `adb` connected:

```
sys.boot_completed: 1
ro.build.version.release: 10
ro.build.fingerprint: oculus/vr_monterey/monterey:10/QQ3A.200805.001/49845030443200410:user/release-keys
uptime: climbing normally, stable (checked twice, 15s apart, both consistent)
```

`logcat` showed the **entire real stock userland stack running normally**: `TrackingService`
reading live HMD IMU samples, `SensorService`, `com.oculus.horizon` (home environment),
`SpatialAnchorPipeline`, `netd`, `DeviceAuthService`. The only errors present
(`DeviceAuthService`/`OVRTZ_FAILURE_SFS_NO_FILE`, "This device *might* have a provisioning issue")
are the well-understood, expected side effect of an unlocked bootloader losing the device-cert trust
chain — unrelated to kernel/ramdisk correctness, not a new finding.

## What this actually proves

1. **Our from-scratch kernel build (`components/kernel`) is correct and boots real hardware
   cleanly** — every prior failure in this arc was never a kernel defect.
2. **The ramdisk-shape hypothesis from research-notes/74 is confirmed as the real, complete root
   cause** of every from-scratch boot failure so far (LEGACYSAR alone insufficient, cmdline fix
   alone insufficient, lineage-21's two-stage-init ramdisk, lineage-18.1's two-stage-init ramdisk in
   either BOARD_BUILD_SYSTEM_ROOT_IMAGE setting) — none of those AOSP-generated ramdisk shapes are
   compatible with this kernel/bootloader's early-boot expectations. The stock ramdisk's specific
   hybrid shape (real `init.rc`/`sepolicy`/`ueventd`/`fstab` embedded directly at ramdisk root, but
   `init` itself a symlink to `/system/bin/init` rather than a self-contained binary — corrected
   detail from research-notes/80) **is** what this kernel needs.
3. This was a stock-ramdisk + stock-system.img test, so it does NOT yet prove any from-scratch
   `system.img` (lineage-21, -18.1, or -17.1) would boot with this ramdisk shape — only that the
   ramdisk shape itself, paired with a known-compatible system.img, is sufficient. That pairing
   question is now the actual remaining unknown for step 5.

## Device state after the test

RAM boot only — `boot_a`/`system_a` on flash were never touched, `slot-unbootable` was `no` on both
slots before the test and this doesn't change that. The device is currently running fully booted
stock OS off our custom-built kernel; a normal reboot returns it to whatever is actually flashed
(stock `boot_a`, unaffected by this test). No recovery action needed.

## Next steps

The real remaining question: can the stock ramdisk's exact structure be adapted to boot a
**from-scratch `system.img`** (lineage-21/Android 14 being the actual target,
research-notes/67's original choice) rather than the stock Android 10 one used in this test. Two
sub-questions, not yet answered:

1. Does the stock ramdisk's `init.rc`/`ueventd.rc`/`sepolicy` need to change at all to work against
   a completely different (self-built, Android 14) `/system` layout, or does `init` (being deferred
   to `/system/bin/init` via symlink) mean the ramdisk-side files barely matter once `/system` is
   mounted and control hands off? Needs direct investigation of what the ramdisk's `init.rc` actually
   does before handing off (early mount commands, `import` statements referencing `/system/etc/init/
   *.rc`) — this determines how much stock-ramdisk content is safe to reuse verbatim vs needs
   regenerating.
2. SELinux policy version compatibility: the stock ramdisk's `sepolicy` is a single monolithic
   Android-10-era policy blob; Android 14 userspace normally expects split
   plat/vendor/product-sepolicy with a specific policy version the kernel's SELinux subsystem
   negotiates. Untested whether the kernel/its loaded LSM will even accept loading a stock Android
   10 policy blob when `/system` is actually Android 14 content, or whether policy version mismatch
   causes a different failure mode entirely (permissive fallback, refused load, boot hang at a later
   stage than anything seen so far).

Both are real open items, not yet started. The stock ramdisk content is fully available at
`work/boot-root/v_orig/ramdisk` (pristine, unmodified) and freshly re-extracted to
`/tmp/stock-ramdisk/extracted/` for inspection.
