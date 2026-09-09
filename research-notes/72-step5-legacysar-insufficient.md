# Step 5: LEGACYSAR fix alone did not resolve the boot failure — 2026-09-09

Continuation of `research-notes/71` on the same day. Tested directly, twice, rather than assumed
sufficient from the strength of the prior match.

## First test attempt was invalid — a methodology error

The first retry after the LEGACYSAR patch only reflashed `boot_a` (the patched kernel), not
`system_a`. `system_a` had already been restored to stock in `research-notes/70`'s recovery, so
that attempt actually tested **patched kernel + stock Meta system** — an inconsistent pairing never
intended, and not a real test of whether the fix helps the actual from-scratch OS. Caught by
checking `system_a`'s live hash before repeating the test (`602653a0...`, matching the stock backup
exactly, confirming the mismatch) rather than assuming the first negative result meant anything.

## Second test: the real combination, still fails

Reflashed both `boot_a` (patched kernel) and `system_a` (the from-scratch LineageOS system.img)
together — the actual intended pairing. Same symptom as `research-notes/70`'s original attempt:
Meta logo briefly, then back to the bootloader. No `adb` reachability across 2 minutes of polling
either time.

**The LEGACYSAR hexpatch was real and necessary (research-notes/21 already established that for
this exact device), but not sufficient on its own for this from-scratch build.** Something else is
also wrong. Recorded honestly as a negative result rather than left ambiguous.

## Recovered cleanly again

Same backups, same verification discipline as `research-notes/70`: both `boot_a` and `system_a`
hashes confirmed matching the known-good backups after restore
(`e534bd75...`/`602653a0...`).

## What's needed next

Without a serial console or any log-capture path from a device that never reaches `adb`, further
diagnosis is guessing without more information. The most valuable next input is what's actually
*visible* on screen during the failed boot — exact sequence, timing, any color/pattern beyond "Meta
logo" — since that's the only observation channel available right now and only the user watching
the physical headset can provide it. Not yet asked in this note; the natural next step before
another flash cycle.
