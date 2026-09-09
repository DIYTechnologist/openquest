# Step 5: first boot attempt failed early, recovery worked cleanly — 2026-09-09

Continuation of `research-notes/69` on the same day. First real test of the from-scratch LineageOS
build on hardware.

## Flash target: the active slot, not a slot switch

Before flashing, clarified with the user which slot should receive the new build, since the two
possible interpretations of "keep a fallback" point at opposite slots: slot A was the currently
*active* slot, matching the verified known-good backup (`research-notes/66`); slot B was *inactive*
with contents that never matched any known backup (`research-notes/66`, presumed leftover from the
QuestStack unlock process). Standard A/B-update practice would flash the *inactive* slot and switch
to it, keeping the active slot as an instant-revert fallback. **The user chose the opposite**: flash
directly into the active slot (A), leaving slot B's already-unknown state untouched and unrelied-
upon as a last resort. Reasoning (the user's): don't make recovery depend on switching to a slot
whose bootability was never verified in the first place — recovery instead depends entirely on this
project's own verified host-side backups, not on-device state.

That choice makes a full, verified backup of the *current* active-slot content a hard prerequisite,
not a nice-to-have — overwriting slot A directly means there's no "switch back" option, only
"reflash from backup." Checked directly before touching anything:

- `boot_a`: already verified against `backups/boot-monterey/new-boot_magisk30.7.img` in
  `research-notes/66`.
- `system_a`: a `system_a.img` backup existed from 2026-08-30 (`backups/
  1PASH9ACHD0215-2026-08-30T15-20-root/`) but hadn't been checked against the *current* live
  partition. Verified directly — two independent 200MB samples (offset 0 and offset 2GB) both
  matched exactly between the backup file and the live `/dev/block/bootdevice/by-name/system_a`.
  The user separately confirmed no system-level changes had happened since that backup was taken.

## Flash and first boot: failed early

```
fastboot flash boot_a   out/target/product/monterey/boot.img     -> OKAY, 2.2s
fastboot flash system_a out/target/product/monterey/system.img   -> OKAY, 42.8s
fastboot reboot
```

Both flashes completed cleanly (no fastboot-level errors). On boot: the Meta logo appeared briefly,
then the device returned to the bootloader — an early boot failure severe enough that the
bootloader's own boot-attempt logic didn't count it as a successful boot. `adb` never saw the
device in this state (polled ~2 minutes, nothing). Not investigated further yet *why* it failed
early — the immediate priority was restoring a known-good device, not diagnosing while the device
sat in a broken state.

## Recovery: clean, first try

Device was conveniently already sitting in fastboot (the failed-boot path returns there
automatically). Reflashed both backups directly:

```
fastboot flash boot_a   backups/boot-monterey/new-boot_magisk30.7.img          -> OKAY
fastboot flash system_a backups/1PASH9ACHD0215-2026-08-30T15-20-root/system_a.img -> OKAY
fastboot reboot
```

Device came back on `adb` normally. Verified, not assumed: `boot_a`'s live hash
(`e534bd75dad028c990bd54b7a53b04d4`) matches the backup exactly, kernel version and build
fingerprint (`oculus/vr_monterey/monterey:10/QQ3A.200805.001/49845030443200410:user/release-keys`)
match stock. Fully recovered, first attempt, no complications — the backup-verification discipline
established before ever touching the device (`research-notes/66` and this note) is exactly what
made this a non-event rather than a real incident.

## What's next

The device tree builds real images but doesn't boot yet — expected for a genuinely first attempt
with zero reference device to compare boot behaviour against. Diagnosing *why* it fails early
(kernel panic, init failure, missing DTB match, SELinux, something else) is real follow-on work,
needs its own investigation before the next flash attempt — not started in this note.
