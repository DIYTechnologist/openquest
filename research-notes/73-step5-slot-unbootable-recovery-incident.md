# Step 5: a real recovery incident — slot-unbootable is separate from partition content — 2026-09-09

Continuation of `research-notes/72` on the same day. After the `androidboot.hardware=monterey`
cmdline test also failed (same symptom), a restore-and-check-pstore cycle turned into a genuine
incident worth recording carefully, not glossed over.

## The incident

1. A `system_a` restore-from-backup failed partway through fastboot (`Error reading sparse file` on
   chunk 2 of 3) — a transient USB/fastboot-protocol issue, not a corrupt source file (the backup's
   own md5 read back fine afterward). `system_a` was left genuinely partially written: not stock,
   not our build, an inconsistent mix.
2. The device began cycling between the Meta logo and the bootloader screen, and **stopped
   enumerating on USB entirely** for a period — no `fastboot devices`, no `lsusb` entry, nothing in
   `journalctl -k` on the host either. Not a host-side problem (the user confirmed the cable was
   never disconnected); the device's own USB stack simply wasn't coming up during this cycling.
3. USB enumeration eventually returned on its own. Cleanly reflashing `system_a` from backup a
   second time (same file, same command) completed with no errors — confirming the first failure
   really was transient, not a reproducible fault in the backup file or the flash process.
4. **Both `boot_a` and `system_a` were now verified byte-identical to the known-good backups, and it
   still would not boot** — same symptom as every from-scratch attempt: Meta logo, then back to the
   bootloader. This was the alarming part: correct, verified content was not enough.

## Root cause: `slot-unbootable`, a separate piece of state from partition content

```
fastboot getvar slot-unbootable:a   -> yes
fastboot getvar slot-retry-count:a  -> 0
```

The bootloader's own A/B boot-control state (tracked independently of what's actually written to
`boot_a`/`system_a`, typically in `misc`) had marked slot A **unbootable** with its retry budget
exhausted, after the string of consecutive failed boots across this session's testing. Once a slot
is flagged this way, the bootloader will not attempt to boot it **at all**, regardless of whether
the partition contents are subsequently fixed — explaining exactly why a verified-correct restore
still failed to boot. This is a different failure class from anything in `research-notes/70`/`72`:
those were the *new build* failing to boot; this was the *bootloader refusing to try*, full stop.

**Fix**: `fastboot set_active a` — resets the retry count and clears the unbootable flag for the
named slot (standard AOSP boot_control behaviour, confirmed working here: `slot-unbootable:a` ->
`no`, `slot-retry-count:a` reset to 7 immediately after). Rebooted; device came up normally, verified
by hash match on both `boot_a` and `system_a` against the known-good backups, plus stock kernel
version and build fingerprint.

## What this means for future testing

**Every few consecutive failed boot attempts, the slot itself can become unbootable independent of
what's flashed to it.** Any future recovery attempt on this device must check `slot-unbootable`/
`slot-retry-count` and run `set_active` if needed — reflashing correct content alone is not
sufficient once this trips. This is now a standing operational fact for this device, not a one-off.

## pstore checked, still empty

After finally getting back to a working boot, checked `/sys/fs/pstore/` immediately: still empty,
across all four failed attempts so far (unpatched kernel, LEGACYSAR-patched, the invalid
mixed-partition test, and `androidboot.hardware=monterey`). This is real, if indirect, evidence the
failure happens early enough that the kernel's console/pstore driver never becomes active — pointing
more toward an ABL-level rejection than a kernel-level panic, though still not confirmed directly.

## Housekeeping

Device fully recovered and verified before writing this note. No further flash attempts made yet —
this note is a checkpoint after a real incident, not a continuation into the next hypothesis.
