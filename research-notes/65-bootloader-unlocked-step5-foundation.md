# Step 5 kickoff: bootloader is genuinely unlocked, correcting research-notes/32 — 2026-09-09

Starting step 5 (`research-notes/18`, the OS swap). Before anything else: `research-notes/32`
concluded the "device is corrupt" boot screen is an unavoidable cosmetic artifact of a fused Meta
key (VB1, no AVB2/vbmeta mechanism), and that a custom signing key cannot be installed. **That
memory is now stale.** The user confirmed **QuestStack**
(`github.com/starseed12345/QuestStack`, GPL-3.0, actively maintained, 178 stars) was used to unlock
this device's bootloader: it chains a root-privilege-escalation exploit ("GhostLock"/ionstack) with
**CVE-2021-1931**, an ABL/fastboot vulnerability, reached by temporarily downgrading the *inactive*
A/B slot to a known-vulnerable firmware build, performing the unlock from there, then restoring the
original slot — a real, documented, community-verified exploit chain, not a guess.

## Verified directly, not taken on faith

Two independent checks, both against the live device:

```
# Android property side
ro.boot.flash.locked=0
ro.boot.verifiedbootstate=orange
sys.oem_unlock_allowed=1

# fastboot ground truth (fastboot getvar all)
unlocked:yes
secure:yes
current-slot:a   slot-count:2
```

`unlocked:yes` from fastboot itself is the strongest possible confirmation — this isn't an inference
from Android-side properties that a compromised or misconfigured system could misreport, it's the
bootloader's own answer. `secure:yes` alongside it means secure boot's chain of trust is technically
still Meta's (VB1, as `research-notes/32` found) — that part of the analysis was correct. What
changes is the *practical conclusion*: an unlocked bootloader accepts custom images **by design**,
not by tolerating a failed check whose limits were never characterised. `research-notes/32`'s
"cannot be signed" framing answered the wrong question — it doesn't need to be signed, it needs the
bootloader unlocked, which it now is.

## Partition table captured while in fastboot (useful for step 5 planning regardless)

```
boot_a / boot_b       0x4000000   (64 MB each)
system_a / system_b   0xA0000000  (2.5 GB each, ext4)
persist               0x2000000   (32 MB, ext4)
```

No `vendor`, `vendor_boot`, or `dtbo` partitions — independently confirms `research-notes/16`'s
finding (no vendor/system split, so no Treble GSI shortcut) from the fastboot side rather than just
Android-side properties. The unlock changes the flashing risk profile; it does not change this
harder constraint. Any new OS still needs a genuinely custom-built system image.

## What this means for step 5

De-risks the mechanics, not the scope. `docs/kernel.md` already has a working, boots-successfully
kernel build (Meta's own source, our container toolchain, byte-identical compiler string) — that
was previously validated only under the old "hope the check is ignored" assumption. The next
concrete, low-risk step is re-validating that same known-good build boots cleanly through the
confirmed-unlocked path, before any new-OS work starts — isolates "does unlocked flashing work" from
"does new work we haven't built yet work," the same discipline every prior step in this project has
used.

The real, much bigger open question — which existing msm8998 AOSP/LineageOS device tree to fork as a
build skeleton (none exist for `monterey` itself, checked via search), how the vendor HAL layer
becomes real services under a new OS's init instead of binder-injection into Meta's still-running
stock processes (how `components/{camera,controllers,tracking}` work today) — is explicitly **not**
resolved here. Recorded as the next real planning task, not rushed into this note.

## Housekeeping

Also fixed, mid-session, unrelated to step 5: the proximity bypass (`am broadcast prox_close`, used
earlier this session for the fb0 feasibility probe) was left engaged with the device sitting unworn
on the desk, lighting the panel unnecessarily. Reverted (`prox_open`) and the display explicitly put
to sleep (`input keyevent 223`) — confirmed `mWakefulness=Asleep`, `Display Power: state=OFF`.

Memory: `quest-bootloader-cannot-be-signed` marked superseded, pointing to the new
`quest-bootloader-unlocked`. No flashing was performed this note — `fastboot getvar all` and a clean
reboot back to Android only.
