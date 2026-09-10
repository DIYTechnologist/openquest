# Step 5 — permissive SELinux test: new failure pattern (repeated cycling), still no adb — 2026-09-09

Continuation of `research-notes/83`. Tried the second planned probe: force
`androidboot.selinux=permissive` via the boot.img cmdline (confirmed via `system/core/init/
selinux.cpp:98-101` that this is checked directly from the kernel cmdline/bootconfig, no system.img
changes needed for this part) to rule SELinux enforcement in or out as the direct cause, combined
with the already critical-flag-patched system.img from research-notes/83 (`ueventd`/
`servicemanager`/`lmkd`).

Also checked, before testing, whether the from-scratch build's APEX packaging could be a factor:
`adbd` in Android 14 ships inside a `.capex` (compressed APEX), unpacked/mounted by `apexd` at
runtime. Extracted `com.android.adbd.capex` directly (`unzip` + `file`) and confirmed the inner
`apex_payload.img` is **ext4/ext2, not EROFS** — ruling out one real concern (this kernel's `.config`
has no `CONFIG_EROFS_FS` at all, not even as a disabled option, so an EROFS-based APEX would have
been a hard, unrecoverable blocker; ext4 payloads work fine with the kernel's existing
`CONFIG_BLK_DEV_LOOP=y`).

## Result: a genuinely different, new failure pattern — but still no adb

Unlike every previous attempt (single crash → park in bootloader), this run **cycled through
repeated boot attempts autonomously**: USB enumerated as the `idProduct=0081` "Android" identity,
ran for ~1m28s, disconnected, then re-enumerated ~22s later as a fresh attempt, before finally
settling back in bootloader for good. Checked `slot-unbootable`/`slot-retry-count` immediately given
research-notes/73's known trap — both stayed clean (`no` / `6`), consistent with the established
finding that `fastboot boot` RAM-boots don't decrement the same retry counter a genuinely-flashed
`boot_a` would.

No working `adb` connection at any point across ~6 minutes of combined tight polling (2-3s
intervals) and live `journalctl -k -f` monitoring. This is now the third consecutive probe
(research-notes/82's two attempts, 83's two attempts, this one) that fails to produce a usable adb
session — the diagnostic ceiling of USB-enumeration-timing-plus-post-hoc-pstore-checks has been
reached.

## Assessment

Neither of tonight's two targeted probes (critical-flag removal, permissive SELinux) produced
a working boot or new diagnostic access, though the *shape* of the failure changed each time
(single-crash vs. repeated-cycling), which at least confirms the system is doing something
different depending on these settings rather than failing at a truly fixed, input-independent point
(e.g. a hard kernel panic before any userspace code runs at all would look identical regardless of
init.rc content). That's weak evidence the failure genuinely is somewhere in Android userspace
init/service startup, consistent with the vendor-HAL-gap theory, but not proof.

Device fully restored and reverified afterward (`sha256sum` match against the gold backup hash,
`sys.boot_completed=1`).

## Recommendation

Both remaining options from research-notes/83 are still open, but tonight's results shift the
balance toward serial/UART console access being the more likely path to real progress — the
software-side debugfs-patch/cmdline-flag approach has now been tried twice without producing usable
signal, and further iteration in that style has a low expected payoff without being able to see
actual boot console output. Serial access requires opening the headset and finding/wiring a UART
header — a hardware step, not something to attempt within this session. Recommend pausing live
device testing on this specific question until that's set up, rather than continuing to spend
flash-and-boot cycles (each with real, if managed, hardware risk) on more blind variants.
