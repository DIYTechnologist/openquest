# Step 5: first successful build — real boot.img and system.img for monterey — 2026-09-09

Continuation of `research-notes/68` on the same day. After the eight bugs documented there (five
getting the device tree recognized/configured, three found deep in compilation), a clean retry with
`BUILD_JOBS=8` and the corrected `BoardConfig.mk` partition sizes ran to completion:

```
#### build completed successfully (05:24 (mm:ss)) ####
```

Real artifacts, checked directly rather than assumed from the exit code:

```
boot.img    21016576 bytes   Android bootimg, kernel (0x8000), ramdisk (0x1000000), page size 4096,
                              cmdline "androidboot.hardware=qcom androidboot.configfs=true loop.max_part=7"
                              -- exactly what BoardConfig.mk's BOARD_KERNEL_CMDLINE specifies
system.img 1296515508 bytes  Android sparse image, 655360 x 4096-byte blocks = 2684354560 bytes
                              uncompressed -- exactly BOARD_SYSTEMIMAGE_PARTITION_SIZE, byte for byte
```

Both real, valid, well-formed Android images — not just "make exited 0."

## What this means for step 5 / Phase 2

The plan's stated Phase 2 milestone was "`device/oculus/monterey/` builds a `system.img` against the
chosen LineageOS source without fatal errors" — **done**. This is the first time this project has
produced a bootable-shaped Android image for a device tree with zero prior existence anywhere
(`research-notes/65`'s search found no `monterey` device tree, upstream or otherwise) — genuinely
new, not adapted from a working reference.

**Not done yet, and explicitly out of scope for this milestone** (per the approved plan): actually
flashing these to the device and confirming `adb shell` is reachable. That's real, meaningfully
greater risk than anything in Phase 0/1 (`research-notes/65`/`66`) — a from-scratch OS attempt, not
a known-good re-flash of something already proven to boot. Needs fresh-verified `boot_a`/`persist`
backups and explicit confirmation before that flash, same discipline `research-notes/66` already
established and is not waived just because the build succeeded.

## Housekeeping

`BUILD_JOBS=8` (`research-notes/68`) held for this run without a repeat OOM — swap pressure was
real but the system found a stable equilibrium rather than crashing. Total elapsed build time for
this specific `bootimage systemimage` invocation: 5m24s, but that's from an already-mostly-cached
state after multiple prior attempts (391 remaining targets, not the ~146000 a from-scratch build
would need) — not representative of a true first-build wall-clock time.
