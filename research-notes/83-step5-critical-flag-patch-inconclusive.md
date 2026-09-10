# Step 5 — patched out critical-crash-panic flags: inconclusive, pstore confirmed non-functional — 2026-09-09

Continuation of `research-notes/82`. Traced the exact crash mechanism via `system/core/init/
service.cpp:358-386`: a `critical`-flagged service crashing more than 4 times before
`sys.boot_completed` makes init call `LOG(FATAL)`, which — since init is PID 1 — triggers a kernel
panic. Also confirmed via `system/core/init/selinux.cpp` that second-stage init loads sepolicy
exclusively from `/system/etc/selinux/` and `/vendor/etc/selinux/` (inside `system.img`), never from
the ramdisk — so the earlier "old ramdisk sepolicy vs Android 14 userspace" concern in
research-notes/81/82 doesn't apply; that file is genuinely inert for this pairing.

## Patched the built system.img directly, no full rebuild needed

Found three services with a plain (non-`shutdown`) `critical` flag in the lineage-21 build's own
generated init scripts: `ueventd` (`hw/init.rc`), `servicemanager` (`servicemanager.rc`), `lmkd`
(`lmkd.rc`). (`zygote`'s equivalent mechanism, `init.zygote64.rc`, defaults its window to `off` and
was already disabled.)

Rather than a full ninja rebuild, patched the already-built `system.img` directly:
`simg2img` to raw ext4 → `debugfs -w` to `rm`+`write` each edited `.rc` file back in place → `debugfs
ea_set` to restore the exact original `security.selinux` xattr (`u:object_r:system_file:s0`,
verified against the original files first) → `e2fsck -fy` clean → `img2simg` back to a flashable
sparse image. Verified every edit by dumping the files back out of the patched image before
flashing.

## Result: inconclusive — no better visibility, no worse either

Flashed the patched image, ran the same diagnostic boot (our kernel + unmodified stock ramdisk)
twice. Neither attempt produced a working `adb` session or any USB activity beyond the same
`idProduct=0081` "Android"-labeled bootloader re-entry seen in research-notes/82. Timing was
notably inconsistent between the two patched attempts (~10-15s before bootloader re-entry once,
~2m38s the other time) — not a fixed watchdog-style timeout, more consistent with variable
crash-restart backoff, but not proof of anything specific.

**This means removing the `critical` flag didn't visibly help** — either the actual failure isn't
one of these three services crash-looping at all (a different, uninvestigated cause), or it is, but
whatever's failing prevents forward progress regardless of whether init would eventually panic on
it. No regression either — can't rule out that the underlying problem is completely unrelated to
the `SVC_CRITICAL` mechanism.

## pstore confirmed structurally non-functional on this device

After restoring stock and getting root again, `/sys/fs/pstore/` exists as a directory but is
**always completely empty** — checked again after this round's ~2m38s attempt (the longest runtime
observed yet across this whole arc) and still nothing. This settles an open question from
research-notes/74/82: pstore's emptiness is not diagnostic of *how early* a given attempt failed —
it appears to never capture anything on this kernel/device regardless of failure type (crash-loop,
panic, watchdog, or otherwise). Stop treating an empty pstore as evidence about failure timing in
future notes on this device.

## Device state

Fully restored and reverified: `sha256sum` of the live `system_a` partition matches the recorded
gold hash (`cf3143347ddd0b649a4a431ad735307ad07d2e7ffda2eadab34f576e112d2e81`) exactly,
`sys.boot_completed=1`, normal stock boot. Safe.

## Honest assessment for next steps

Further root-causing this specific crash via more RAM-boot trial-and-error has hit diminishing
returns without actual boot-time log visibility. Two real options, neither started:

1. **Serial/UART console access** — the only way to see boot output directly rather than inferring
   from USB enumeration timing and post-hoc pstore checks (which has now been shown not to work on
   this device). Requires opening the headset and finding/wiring a UART header — a real hardware
   step, not pure software investigation.
2. **Build a deliberately more verbose/permissive debug variant** of the lineage-21 system.img —
   e.g., force `androidboot.selinux=permissive` via cmdline (ruling SELinux in/out entirely rather
   than guessing), or patch `adbd`'s own service definition to start as early and unconditionally as
   possible (`class core`, no dependency on later triggers) to maximize the chance of a working
   `adb` connection during whatever window exists before the eventual reset — worth trying before
   committing to hardware UART work, since it stays within tonight's existing toolchain (same
   debugfs-patch technique just used).

Given the session's already-real hardware risk this evening (one USB/bootloader hang requiring a
full physical power cycle) and two more flash-and-boot cycles just now, this is a reasonable point
to pause live device testing and let the user decide which of the two directions above to pursue
next, rather than continuing to iterate blind.
