# Step 5: lineage-18.1 bring-up reference kicked off — 2026-09-09

Continuation of `research-notes/74` on the same day, following the user's chosen strategy: get
`lineage-18.1` (Android 11) booting first, using its native support for the old monolithic ramdisk
this kernel likely needs, as a proven reference before hand-building an equivalent ramdisk for the
`lineage-21` (Android 14) tree. Sequencing rationale (the user's): validates the kernel/hardware
bring-up on a natively-supported architecture, de-risking the harder Android 14 path rather than
reverse-engineering the ramdisk shape blind.

## New, separate build environment

Same pattern as `components/os`'s existing `lineage-21` setup, deliberately kept **separate**, not
reusing any of it:

- `build/containers/lineageos18-build/`: its own container (Ubuntu 20.04, OpenJDK 11 — Android
  11-era AOSP is documented as wanting OpenJDK 9, long EOL on Ubuntu 20.04's own repos; starting
  with 11 since LineageOS has historically backported newer-JDK acceptance to older branches, and
  adjusting if the real build complains rather than pre-emptively chasing an EOL package).
  `repo`/`git-lfs` included, same as the lineage-21 container (missed on the first attempt there
  too, same fix applied here from the start this time).
- `work/lineageos18/`: a second, separate source tree (gitignored, matches the `/work/` convention)
  — `work/lineageos` (the Android 14 tree, 244GB) is untouched, kept for the later hand-built-ramdisk
  phase.
- `components/os/device-monterey-18/`: a new device tree, adapted from `device-monterey/` but for
  Android 11-era conventions — critically, **`BOARD_BUILD_SYSTEM_ROOT_IMAGE := true`**, the one
  config value this whole reference build exists to test, since it's `KATI_obsolete_var` on
  lineage-21 (`research-notes/72`). Also: `add_lunch_combo` (still current on this AOSP version,
  unlike lineage-21's obsolete usage, `research-notes/68`), two-part lunch combos (no release name),
  `PRODUCT_SHIPPING_API_LEVEL := 30`. Kept the `androidboot.hardware=monterey` cmdline fix
  (`research-notes/73`) since it's independently verified correct against the live stock boot_a,
  regardless of the ramdisk-architecture question.
- `components/os/Makefile` gained `init-18`/`sync-18`/`lfs-pull-18`/`device-tree-link-18`/
  `kernel-prebuilt-18`/`lunch-18` targets. `container.mk`'s wrapper only supports one
  `CONTAINER_IMAGE` for the whole file, so these invoke `podman` directly against
  `quest-lineageos18-build` rather than fighting that — an explicit rule with a real recipe takes
  precedence over container.mk's generic `%:` pattern rule regardless of include order, confirmed
  working (`make init-18` correctly used the new container, not the lineage-21 one).
- Same patched kernel (`components/os/patch_legacysar_kernel.py`) as the lineage-21 tree — one
  source of truth, copied into `device-monterey-18/prebuilt/` (kept separate from
  `device-monterey/prebuilt/` so neither tree's build can race the other).

**Repeated the exact mistake from the first lineage-21 container** (`research-notes/68`): the first
Dockerfile draft omitted the `repo` launcher entirely (`bash: repo: command not found`). Fixed
immediately, same one-line addition as before — recorded so it's visible this wasn't caught by
having done it once already; each container needs its own complete checklist, not assumed carried
over.

## Status

`repo init -b lineage-18.1` succeeded. `repo sync -c -j4 --no-clone-bundle` running in the
background as of this note — expect the same class of issues as the lineage-21 sync
(`research-notes/67`/`68`: possible LFS-pointer gaps, long duration) until proven otherwise.

## What's next

Once synced: `make -C components/os lunch-18` — expect real iteration on the device tree draft
(same discipline as `research-notes/68`'s five config/discovery bugs) before a first real build,
then a flash-and-boot test using the same careful backup-verification and `slot-unbootable`-aware
recovery discipline established in `research-notes/70`/`73`.
