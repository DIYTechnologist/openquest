# Step 5 — lineage-18.1 reference build done, but overturns research-notes/74's premise — 2026-09-09

Continuation of `research-notes/77`. Two more real bugs found and fixed getting `lunch-18` to a
clean, successful `boot.img`/`system.img` build (three build attempts total, ~35min / 40s / 1h35m).
The result **overturns the working hypothesis from research-notes/74** — a real, useful outcome,
but not the one expected: lineage-18.1 (Android 11) cannot produce the stock device's actual
ramdisk shape at all, regardless of `BOARD_BUILD_SYSTEM_ROOT_IMAGE`.

## Bug 3 — `BOARD_BUILD_SYSTEM_ROOT_IMAGE := true` has the OPPOSITE effect from what research-notes/74 assumed

First `lunch-18` build (35 min) succeeded, but `unpack_bootimg` on the resulting `boot.img` showed a
**literal 0-byte ramdisk** and `out/target/product/monterey/root/` held only `default.prop` (a
symlink) + `fstab.monterey` — nothing else.

Checked directly against `build/make/core/main.mk:1618`: `ifneq ($(BOARD_BUILD_SYSTEM_ROOT_IMAGE),
true)` gates whether `INSTALLED_RAMDISK_TARGET` (the classic full ramdisk) gets built **at all**.
Setting it `true` is System-As-Root mode: a deliberately near-empty ramdisk, with the real root
content living in `system.img` instead. research-notes/74 had the causality backwards — it assumed
`true` meant "the old monolithic style"; it's the reverse. Fixed: unset the flag entirely in
`device-monterey-18/BoardConfig.mk`. Rebuild (40s, incremental) did produce a real `ramdisk.img`
target this time, but decompressed to 256 bytes — a technically-valid but empty cpio (TRAILER record
only, no files).

## Bug 4 — no `core_minimal.mk`/`aosp_base.mk` inheritance, so no base packages exist to populate the ramdisk

`core_64_bit.mk`'s own header comment says it "must come before the inheritance chain that leads to
core_minimal.mk" — implying something else has to actually complete that chain. Our
`device-monterey-18/lineage_monterey.mk` never did (unlike the lineage-21 tree's equivalent file,
which inherits `aosp_base.mk`). Without it, `PRODUCT_PACKAGES` stays essentially empty — no
`init`/`sepolicy`/`toybox`/`adbd` etc. get declared, so there's nothing to put in the ramdisk even
once the target exists. Fixed: added `$(call inherit-product,
$(SRC_TARGET_DIR)/product/aosp_base.mk)`, matching the lineage-21 tree. Third build (1h35m, the
`aosp_base.mk` chain pulls in frameworks/apex/full app set, ~65700 ninja targets vs ~31900 before)
succeeded cleanly.

## The real finding: even now, the ramdisk is NOT what research-notes/74 needs

Unpacked the resulting `boot.img`. Ramdisk is now a real, substantial 1.8MB uncompressed cpio (up
from 256 bytes) — but its actual content is:

```
debug_ramdisk/  dev/  init (1.8MB binary)  mnt/  proc/  sys/
```

**No `init.rc`. No `sepolicy`. No `fstab.monterey` at ramdisk root. No `bin -> /system/bin` symlink.**
Just a single combined `init` binary and empty mountpoint directories — this is the modern two-stage-
init **first-stage** ramdisk shape (the same shape the lineage-21 tree produced, research-notes/74),
not the stock device's self-contained monolithic root.

Checked why directly against `build/make/core/Makefile:4924-4925` (lineage-18.1, i.e. Android 11):

> `BOOT/RAMDISK also exists and contains the first stage ramdisk if not using
> BOARD_BUILD_SYSTEM_ROOT_IMAGE.`

**AOSP itself calls the non-SAR ramdisk "the first stage ramdisk" by this version.** Two-stage init
is unconditional, baked into `system/core/init`'s own logic at Android 11 — `BOARD_BUILD_SYSTEM_
ROOT_IMAGE` only selects *which* of two already-two-stage-init ramdisk shapes you get (SAR: merge at
boot into system.img; non-SAR/legacy: separate first-stage-only boot ramdisk, real init.rc lives in
system.img either way). Neither setting produces the stock device's actual layout (real init.rc/
sepolicy/fstab/symlinks living directly in the boot ramdisk, no handoff to a second stage at all).

**This overturns research-notes/74's whole premise**, not just the flag's polarity: lineage-18.1
(Android 11) is *already too new* to build a genuine single-stage-init ramdisk via standard `make
bootimage`, regardless of any BoardConfig flag. Getting the stock device's real shape would require
an AOSP branch from before two-stage init became unconditional — meaningfully older than Android 11,
likely Android 8.1/9-era (lineage-15.1/16.0), which is consistent with research-notes/74's own
observation that the stock kernel is "Android 8/9-era."

## Standing rule learned this session

- **Don't trust an inherited assumption about what an AOSP board-config flag does — read the actual
  Makefile logic it gates.** `BOARD_BUILD_SYSTEM_ROOT_IMAGE`'s meaning, and even whether it's
  relevant at all to the specific behavior being chased, changed across AOSP versions in ways that
  contradicted the natural reading of its name. Verify against `build/make/core/*.mk` on the actual
  synced tree in question, not against general AOSP knowledge or what an older/newer sibling tree in
  this same project did.

## Next step — needs a decision before spending another multi-hour build cycle

Three options, not yet chosen:
1. Try an even older AOSP branch (lineage-16.0 / Android 9, or lineage-15.1 / Android 8.1) as the
   new reference target for the real single-stage-init ramdisk shape — repeats this session's whole
   arc (new container/tree/device-tree-clone, expect a similar 3-5 bug-fixing iteration) one rung
   further back.
2. Abandon the "build a reference first" strategy and hand-build the monolithic ramdisk directly for
   the lineage-21 (Android 14) tree instead, using the *stock* ramdisk's actual real content
   (research-notes/74 already unpacked and characterized it fully) as the template, skipping the
   AOSP-produced-reference step entirely — never actually required an AOSP build to produce this
   shape, since two-stage init has been unconditional for a long time regardless.
3. Reconsider whether patching the two-stage-init handoff itself (rather than avoiding it) is more
   tractable now that its exact shape/behavior on this kernel is well understood.

No lunch-18/-21 build has been kicked off pending this decision.
