# Step 5 Phase 1: unlocked flash/boot loop validated — 2026-09-09

Continuation of `research-notes/65` on the same session. With the bootloader confirmed genuinely
unlocked, validated the flash mechanism itself before building anything new on it — isolating "does
unlocked flashing work" from "does new work we haven't built yet work."

## Method: reflash the exact image already running, not new content

Deliberately chose the lowest-risk possible test: `backups/boot-monterey/new-boot_magisk30.7.img`
(md5 `e534bd75...`) matched the currently-installed `boot_a`'s own hash exactly (confirmed by
`dd`-ing the live partition and hashing it before touching anything). Reflashing it is zero content
risk — it exercises `fastboot flash` + reboot under the unlocked bootloader without also
reintroducing the instrumented kernel's documented UFS-wedge history (`docs/kernel.md`), which would
have muddied what this test was actually checking.

## Result

```
fastboot flash boot_a new-boot_magisk30.7.img   -> OKAY, 2.27s total
fastboot reboot                                  -> device back on adb within ~20s
/proc/version                                    -> unchanged, as expected
boot_a hash post-flash                           -> e534bd75... (unchanged, as expected)
trackingservice / sensors-hal                     -> both running
dumpsys tracking                                  -> Valid: Yes, 3DOF (correct resting state, not
                                                      proximity-bypassed)
```

Clean, fast, no warnings or errors beyond the standard expected unlocked-bootloader notice. The
flash mechanism works as designed now that the bootloader is genuinely unlocked — this was the
thing Phase 1 needed to confirm before any new-OS work starts.

## Backup gap noted, not resolved

While checking `boot_a` against known backups, also hashed `boot_b` (the inactive slot): it matches
**none** of the documented backups (stock/magisk27/magisk30.7). Likely a leftover from QuestStack's
unlock process (which temporarily uses the inactive slot to reach the vulnerable ABL). Asked the
user whether they ran QuestStack themselves (its own backup system would have the authoritative
original) — they weren't sure and asked not to chase it further right now, since `boot_a` (the slot
that actually matters for booting and for this validation) is accounted for. Recorded as a known,
accepted gap, not silently ignored: if `boot_b` is ever made the active slot for any reason, its
state is unverified.

## What's next

Phase 0/1 of the step-5 kickoff plan are done. Phase 2 — the real architectural work (choosing an
msm8998 AOSP/LineageOS base to fork as a skeleton, since no `monterey` device tree exists anywhere;
how `components/{camera,controllers,tracking,kernel}` become real vendor HAL services under a new
OS's init instead of binder-injection into Meta's still-running stock processes) — is explicitly not
started. It's a materially bigger planning task and needs its own dedicated pass.
