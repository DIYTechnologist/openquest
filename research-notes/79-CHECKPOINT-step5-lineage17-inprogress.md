# CHECKPOINT — step 5, lineage-17.1 (Android 10) bring-up sync in progress — 2026-09-09

Read this first on resume; supersedes `research-notes/76`'s "next step" section (everything else in
76 about the project as a whole still holds). Covers `research-notes/77`/`78` plus the pivot that
followed.

## What happened since `research-notes/76`

1. The `repo sync` for lineage-18.1 that `research-notes/76` left running finished cleanly.
2. Found and fixed two real Makefile bugs before the first lineage-18.1 build attempt could even
   start meaningfully (`research-notes/77`): `LFS_DIRS` and `BUILD_JOBS` were both defined *inside*
   the `ifeq ($(IN_CONTAINER),1)` block in `components/os/Makefile`, invisible to the host-side `-18`
   targets that reference them when building their `podman run` command lines. `lfs-pull-18` was a
   silent no-op (`for d in ; do`); `lunch-18` ran ninja with a bare `-j` (unbounded — in practice
   settled at ~nproc concurrency, not a runaway, but don't rely on that happening again). Both fixed
   by moving the variable definitions to top level.
3. Got a full, successful lineage-18.1 build (`research-notes/78`), but it **overturned research-
   notes/74's premise** rather than confirming it: `BOARD_BUILD_SYSTEM_ROOT_IMAGE := true` turned out
   to mean the *opposite* of what 74 assumed (system-as-root = near-empty ramdisk, not "the old
   monolithic style"). Fixing that polarity plus a missing `aosp_base.mk` inherit (no base packages
   → nothing to put in the ramdisk) got a real, substantial ramdisk built — but its content is a bare
   two-stage-init first-stage shape (just an `init` binary + mountpoints, no `init.rc`/`sepolicy`/
   symlinks), because **AOSP's own Makefile at lineage-18.1 (Android 11) calls the non-SAR ramdisk
   "the first stage ramdisk" unconditionally** — two-stage init is baked in at that version
   regardless of the SAR flag. Confirmed by reading `build/make/core/Makefile:4924-4925` directly on
   the actual synced tree, not by general AOSP knowledge.
4. Asked the user how to proceed given this. **User's call: match the AOSP branch to the stock
   device's own actual Android version rather than guessing an older one blind.** Checked
   `research-notes/03`/`17`: the stock device is confirmed running **Android 10** (not 8/9 as
   `research-notes/74` had guessed), kernel 4.4.205. That maps to **lineage-17.1**.

## Current state — lineage-17.1 bring-up, mid-flight

- `components/os/Makefile`: added `init-17`/`sync-17`/`lfs-pull-17`/`device-tree-link-17`/
  `kernel-prebuilt-17`/`lunch-17`, mirroring the `-18` targets exactly. **Reuses the same
  `quest-lineageos18-build` container image** (Ubuntu 20.04 + OpenJDK 11) rather than building a
  third one — that toolchain already proved sufficient for a newer AOSP branch than its own docs
  recommend, worth trying as-is here before assuming a JDK/toolchain mismatch.
- `components/os/device-monterey-17/`: new device tree, copied from the now-working
  `device-monterey-18/` and adjusted (`PRODUCT_SHIPPING_API_LEVEL := 29`, comments updated). Left
  `BOARD_BUILD_SYSTEM_ROOT_IMAGE` unset (matching -18's working config) and **deliberately left the
  `hardware/qcom-caf/common/common.mk` inherit-or-not decision as a TODO** to check against the real
  lineage-17.1 source once synced — research-notes/78 explicitly found guessing this from the
  sibling tree's answer would be unreliable (18.1 doesn't have the file even though 21 does, the
  reverse of the obvious guess).
- **`init-17`/`sync-17` were kicked off in the background right before this checkpoint was written**
  (`nohup make -C components/os init-17 sync-17 > /tmp/sync17.log 2>&1 &`, disowned — NOT tracked by
  any tool's background-task notification system, same pattern as the lineage-18.1 builds). Check
  `/tmp/sync17.log` and `du -sh work/lineageos17` on resume.
- **Disk headroom is getting tighter**: 315G free as of this checkpoint, with `work/lineageos`
  (244G) and `work/lineageos18` (193G) already on disk including their `out/` build artifacts. A
  third full tree+build should still fit, but if disk gets tight, `work/lineageos18/out` is the
  first reasonable thing to clear (the lineage-18.1 investigation is fully written up in
  research-notes/77/78 already, doesn't need to stay built) — do NOT touch `work/lineageos` (the
  actual Android 14 target tree) without checking with the user first.

## The open question this build exists to answer

Does lineage-17.1 (Android 10) — one AOSP branch older than 18.1, and the version the stock device
itself actually runs — still support genuinely single-stage init (a monolithic ramdisk with real
`init.rc`/`sepolicy`/`fstab`/symlinks embedded directly, no handoff to a second stage), or is
two-stage init already unconditional there too? **Don't assume either way** — once this tree is
synced, grep `build/make/core/Makefile` on the real lineage-17.1 source directly for how it talks
about `BOARD_BUILD_SYSTEM_ROOT_IMAGE`/"first stage ramdisk" (the same technique that answered this
definitively for 18.1 in research-notes/78), and/or just build it and unpack the resulting
`boot.img`'s ramdisk to look for a real `init.rc` at its root, before investing in interpreting
config semantics.

## Next steps on resume, in order

1. Check `/tmp/sync17.log` — is `sync-17` still running, done, or failed? (`du -sh
   work/lineageos17`, `podman ps` for a container running `sync -c -j4` against
   `work/lineageos17/.repo`, same liveness-check technique as `research-notes/76`: real CPU-time
   growth on `index-pack`/`git fetch-pack` processes across two samples, not just instantaneous
   stats.)
2. Once synced: `git lfs pull` will already run automatically as part of `sync-17`'s recipe
   (`lfs-pull-17` chained in) — verify the four `webview.apk` files are real (`file` should say
   "Android package (APK)", not "ASCII text") rather than assuming the fix from research-notes/77
   carried over correctly.
3. Check `hardware/qcom-caf/common/common.mk` existence directly on the synced tree; update
   `device-monterey-17/lineage_monterey.mk`'s TODO accordingly before the first `lunch-17` attempt
   (don't just let it fail first if it's cheap to check).
4. `make -C components/os lunch-17` — expect a config-fixing iteration similar to research-notes/77
   (different repo, different branch, near-certainly some new adjustment needed) before a real ninja
   build starts.
5. Once it builds: unpack `boot.img`, check the ramdisk's actual content (real `init.rc` at root?
   `bin -> /system/bin` symlinks? one combined `sepolicy`?) against the stock ramdisk's structure
   already fully characterized in `research-notes/74`, before concluding anything either way.
6. If lineage-17.1 genuinely produces the monolithic shape: that becomes the reference for hand-
   building an equivalent ramdisk for the lineage-21 (Android 14) tree (still not started). If it
   doesn't either: the three options from research-notes/78 (go to lineage-16.0/15.1; skip the AOSP
   reference entirely and hand-build directly from the already-characterized stock ramdisk;
   reconsider adapting to two-stage init instead) are back on the table, now with lineage-17.1's
   result as an added data point.
