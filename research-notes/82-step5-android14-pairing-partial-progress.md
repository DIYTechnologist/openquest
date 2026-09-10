# Step 5 — stock ramdisk + our Android 14 system.img: real progress, not yet a boot — 2026-09-09

Continuation of `research-notes/81`. Tested pairing the exact stock ramdisk (proven to boot in 81)
against our from-scratch lineage-21 (Android 14) `system.img` instead of the stock one. User
approved flashing `system_a` for this test given the verified backup.

## Mechanism traced before testing

Read `system/core/init/main.cpp` on the real lineage-21 source directly: `main()` dispatches to
`SecondStageMain` only if `argv[1] == "second_stage"`; otherwise it always runs `FirstStageMain`.
Since the stock ramdisk's `/init` is a **symlink** to `/system/bin/init` (not a real binary,
correcting research-notes/74's original description), the kernel's initial `execve("/init")` runs
whatever real init binary lives in `system.img` — but it starts as **first-stage**, still rooted in
the ramdisk's own tmpfs. `fs_mgr`'s `GetFstabPath()` (`system/core/fs_mgr/libfstab/fstab.cpp:510`)
checks both bare-ramdisk-root and `/first_stage_ramdisk/`-nested locations for `fstab.<hardware>`,
so the fstab-discovery nesting question I initially suspected isn't the likely culprit. Second-stage
init (once re-exec'd with `second_stage`) hardcodes `/system/etc/init/hw/init.rc`
(`init.cpp:341-343`) — **not** anything at ramdisk root — so the stock ramdisk's own top-level
`init.rc`/`sepolicy`/`ueventd.rc` files are actually vestigial for a normal two-stage boot; they're
only relevant if first-stage init falls back to some very early, pre-mount behavior. The real risk
this predicted: whatever init binary system.img provides (Android 14, in this test) has to
correctly complete first-stage mount and handoff using the OLD Android-10-era ramdisk content it's
handed, before it ever gets to read anything from its own (Android 14) system partition.

## Test and result: further than any AOSP-ramdisk attempt, but not a stable boot

Backup verified against live device before flashing (`sha256sum` of `/dev/block/bootdevice/by-name/
system_a` via root matched the recorded gold hash exactly). Flashed `work/lineageos/out/target/
product/monterey/system.img` (the lineage-21 build) onto `system_a`, then `fastboot boot`'d the same
test image from research-notes/80/81 (our kernel + unmodified stock ramdisk).

**Two attempts, both qualitatively different from every from-scratch full-image attempt before this
session** (which always showed "Meta logo briefly, straight back to bootloader," `pstore` always
empty):

- Attempt 1: ~20s of silence, then a real USB gadget enumeration (`idProduct=0081`, "Android"), then
  disappeared again ~26s later. `fastboot devices` then showed the device back in bootloader.
- Attempt 2: ~2.5 minutes of total USB silence (much longer stall than attempt 1 or than any
  AOSP-ramdisk failure), then the same `idProduct=0081` enumeration appeared — but this time
  `fastboot getvar all` against it returned genuine bootloader-only variables (`arb-index`,
  `hw-revision`, etc.), confirming it had landed back in the **bootloader**, not a working userspace
  ADB session, despite the misleadingly-identical USB descriptor to the one that WAS genuine ADB in
  research-notes/81's successful stock-pairing test.

Neither attempt produced a working `adb` "device" session, so no logcat could be pulled from the
crash itself.

## pstore stayed empty even here — a real, informative negative result

After restoring stock (below) and getting a clean root shell, checked `/sys/fs/pstore/`:
completely empty, no files at all — same as every prior attempt in research-notes/74. Given this
attempt ran for ~2.5 minutes with real USB/userspace activity (unlike the near-instant "Meta logo"
failures), an empty pstore after that much runtime is itself a data point: it's more consistent with
a **hardware watchdog reset** (SoC-level, bypasses the kernel's own panic/pstore-write path entirely)
than a kernel panic (which, on a device with pstore actually wired up, would normally leave
something behind after 2.5 minutes of uptime). Not confirmed — no serial console access to verify
directly — but the leading working theory going into any follow-up.

**Plausible root cause, not yet investigated**: `quest-os-replacement-blockers` (memory) already
established this device has no vendor partition — any from-scratch OS build's vendor HAL layer is
necessarily incomplete/stubbed relative to Meta's real blobs. A hung service repeatedly retrying a
nonexistent vendor HAL during Android 14's second-stage init would be a very plausible way to
eventually trip a hardware watchdog after a couple of minutes, without ever reaching a state where
adbd's USB gadget config completes enough for the host to get a "device" (vs. just seeing the raw
USB descriptor).

## An unrelated but real hardware hiccup during this test, worth recording

Mid-test, a `fastboot flash` (restoring the stock backup) stalled at ~80KB read of a 2.68GB source
(`/proc/<pid>/io` showed the transfer genuinely frozen, not just slow) and then the device stopped
responding to any real fastboot command (`getvar` hung/timed out repeatedly) while still enumerating
at the raw USB descriptor level (`fastboot devices` kept succeeding — a lighter check than an actual
protocol round-trip). A host-side `usbreset` did not fix it. **Only a genuine full power-off (screen
going completely black) fixed it** — an earlier attempt at a quicker reset left the device
re-enumerating with the exact same "already wedged" signature. New standing rule, added to memory:
if fastboot enumerates but any real command (not just `fastboot devices`) hangs, don't trust a quick
power-button reset or a host-side `usbreset` alone — insist on a full cold power-off before
retrying.

## Device fully restored and verified

- `fastboot flash system_a` with the verified stock backup, `fastboot reboot`, confirmed via
  `adb shell su -c sha256sum` against the live device: `cf3143347ddd0b649a4a431ad735307ad07d2e7ffda2eadab34f576e112d2e81`
  — exact match to both the pre-flash live-device check and `research-notes/03`'s recorded gold
  hash.
- `sys.boot_completed=1`, `ro.build.fingerprint` matches stock exactly. Device is safe, healthy,
  back to its normal stock state.

## Next steps

1. Root-cause the eventual reset more precisely — needs either serial/UART console access (not
   currently available) or a way to catch a live `adb` session during the ~20s-2.5min window before
   whatever kills it fires. Worth trying `adb connect <ip>:5555` in a tight loop in parallel with USB
   polling next attempt, in case wifi-adb comes up before/instead of USB-adb.
2. If the watchdog/vendor-HAL theory is right, the fix isn't ramdisk content at all — it's making
   the from-scratch Android 14 build's init.rc/service definitions tolerant of missing vendor HALs
   (disable/stub the services that would hang waiting on them) rather than anything ramdisk-shape
   related. This would be a real, scoped follow-up to `quest-os-replacement-blockers`'s already-known
   vendor-HAL gap, not a new problem.
3. Alternatively: try the SAME pairing test against the lineage-18.1 (Android 11) system.img instead
   of lineage-21 (Android 14) — a smaller version gap from the ramdisk's Android-10-era content might
   surface a different (more or less severe) failure mode, useful triangulation data regardless of
   outcome.
