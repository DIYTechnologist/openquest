# Step 5: found a likely root cause — modern two-stage-init ramdisk vs. this kernel's legacy expectations — 2026-09-09

Continuation of `research-notes/73` on the same day, using the recovered device (no new flash yet).
With the `androidboot.hardware=monterey` cmdline fix also ruled out in isolation and `pstore`
staying empty across every attempt, went back to first principles: unpack both boot images with
the real AOSP tool (`unpack_bootimg`) and compare every section directly, not just the cmdline.

## DTB: structurally similar, probably not it

Both stock and our kernel embed the DTB the same way — the standard Linux ARM64
"append every board-variant DTB, let early boot match by hardware ID" convention (repeated
`d00dfeed` magic entries, `vs1 proto1`, `vs1 proto2`, ... in the same order), not a Qualcomm QCDT
table. Sizes differ modestly (5.31MB stock vs 5.45MB ours) but the structure and board-variant set
look equivalent. Downgraded from leading suspect to unlikely.

## Ramdisk: completely different architecture, not just a size difference

Extracted both with `cpio` for a direct listing, not just comparing byte counts:

**Stock** (`~10.9MB`) — a monolithic, pre-two-stage-init ramdisk. Contains a real `init` binary
(199KB, old-style), `init.rc`, `ueventd.rc`, `ueventd.monterey.rc`, a single combined `sepolicy`
file, `fstab.monterey` at the root, ext `verity_key`, and direct symlinks —
`bin -> /system/bin`, `vendor -> /system/vendor`, `product -> /system/product`. This ramdisk *is*
effectively the root filesystem; `/system` is just where the rest gets mounted in.

**Ours** (`~1.6MB`) — the modern AOSP two-stage-init ramdisk. Just `init` (3.6MB, the unified
modern binary), `first_stage_ramdisk/`, `second_stage_resources/`, `metadata/`, a handful of empty
mount-point directories. No `init.rc`, no `sepolicy`, no `fstab` at the root — all of that is
expected to live in `system_a` instead, handed off to by first-stage init.

These are not variations on the same theme — they're two different Android boot architectures.
`BOARD_BUILD_SYSTEM_ROOT_IMAGE` (the AOSP flag that used to select the older monolithic style) is
now `KATI_obsolete_var` in this AOSP version (checked directly, `research-notes/72`'s log) — the
modern two-stage shape is the *only* one this build system produces anymore.

## Why this is the leading hypothesis for the boot failure

This device's kernel is genuinely old — Android 8/9-era (`4.4.205`), and `research-notes/21`
already established it needs the legacy `want_initramfs` SAR hexpatch just to use a ramdisk at all
rather than mounting system-as-root directly. A kernel from that era, on hardware this specific
(`monterey`, never a mainline-supported device, no upstream two-stage-init port ever attempted),
was very plausibly never built or tested against a modern two-stage-init handoff at all. `pstore`
staying empty across four different failed attempts (unpatched kernel, LEGACYSAR-patched,
LEGACYSAR + corrected cmdline) is consistent with a failure early enough that first-stage init
never gets far enough to initialize console/pstore — which is exactly what would happen if
first-stage init itself can't make sense of a boot flow this kernel's early code doesn't expect.

**Not proven** — this is the leading hypothesis based on the strongest, most concrete structural
evidence found so far, not a confirmed root cause. No direct test of "does a monolithic ramdisk
fix it" has been run yet.

## What this means for the project's own choices, honestly

The user picked LineageOS 21 (Android 14) specifically for better Vulkan/OpenXR platform support
for Monado/ALVR later (`research-notes/67`). If this hypothesis is right, Android 14's build system
structurally cannot produce the ramdisk shape this kernel needs — `BOARD_BUILD_SYSTEM_ROOT_IMAGE`
being obsolete isn't a config value to flip, the whole two-stage-init mechanism replaced it project-
wide. That's a real, bigger-than-a-BoardConfig-tweak decision: either target an older LineageOS
branch (`research-notes/67` already found `lineage-18.1`, Android 11, has active msm8998 device
trees, from *other* phones sharing the SoC but not this kernel's specific legacy-SAR quirk — still
untested for that quirk specifically) or find a way to make two-stage init work despite the kernel's
age (unclear if possible, not researched). Recorded as an open strategic fork, not decided here.
