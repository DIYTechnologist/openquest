# CHECKPOINT — step 5 (OS swap) underway, lineage-18.1 bring-up sync in progress — 2026-09-09

Read this first on resume. Standing rules from `research-notes/52` still hold except where
corrected below. This checkpoint covers everything since `research-notes/52`/`53` — a very large
session: closed out controller-tracker/motion-to-photon/distortion-mesh loose ends, then did
essentially all of step 5 (the OS swap) up to a real, still-open boot-failure investigation.

**Project goal:** replace Meta's blobs on an EOL Quest 1 (`monterey`, msm8998) with an open VR
stack, so the device can run a modern OS and general APKs (ALVR/SteamVR). Strategy: replace
services one at a time on the stock OS with a known-good fallback, *then* swap the OS.

## Scoreboard

| Step | State |
|---|---|
| 1 — Direct-kernel camera | DONE |
| 2 — Ground truth vs Meta | DONE (7.6cm in-place, 11.7cm/60m room-scale) |
| 3 — Controllers | input DONE; 6DoF: working v1 tracker, sub-cm when correct, ~80% correct, drift-lock mechanism understood not fixed |
| 4 — `trackingservice` in place | injection proven; motion-to-photon **blocked** (no software path exists, needs a photodiode you don't have, `research-notes/62`) |
| 5 — OS swap | **actively in progress**, see below — builds real images, doesn't boot yet, root cause under active investigation |
| 6 — Display/compositor | characterisation done; distortion-mesh converter fidelity verified but absolute accuracy blocked (same wall as #4, `research-notes/63`) |

## Step 5 — exactly where things stand

**Bootloader is genuinely unlocked** (QuestStack/CVE-2021-1931, confirmed via `fastboot getvar
unlocked:yes`, `research-notes/65`) — supersedes the old `quest-bootloader-cannot-be-signed` memory,
now pointed at `quest-bootloader-unlocked` instead.

**`components/os/`** holds a from-scratch `device/oculus/monterey` tree building against
**LineageOS 21 (Android 14)** — no such device tree exists anywhere upstream. It builds real, valid
`boot.img`/`system.img` (`research-notes/69`) after **eight bugs** fixed getting there
(`research-notes/68`: five in device-tree recognition/config, three in compilation — a real
host-wide OOM at `-j16` fixed with `BUILD_JOBS=8`, silently-unfetched Git-LFS content needing an
explicit `git lfs pull`, hex-vs-decimal partition sizes breaking a Python parser).

**It does not boot.** Every attempt: Meta logo briefly, then back to bootloader. Diagnosed two real,
necessary-but-not-sufficient fixes so far:
- `research-notes/71`: this is a legacy system-as-root (SAR) device (`research-notes/21` found this
  months ago for a different reason) — kernel needs `skip_initramfs` hexpatched to
  `want_initramfs` or it never uses the ramdisk at all. `components/os/patch_legacysar_kernel.py`
  does this permanently now, wired into the `kernel-prebuilt` Makefile target. **Necessary, not
  sufficient** — still doesn't boot with this alone (`research-notes/72`).
- `research-notes/73`: `androidboot.hardware=monterey` (not generic `qcom`), found by direct
  `unpack_bootimg` comparison against the live stock `boot_a`. Also not sufficient alone.
- **`research-notes/73` also documents a real recovery incident**: after several failed boots, the
  bootloader marks the active slot **unbootable independent of partition content** —
  `fastboot getvar slot-unbootable:a` was `yes` even after reflashing verified-correct backups, and
  it would not boot until `fastboot set_active a` cleared it. **Standing rule for any future
  recovery on this device**: check `slot-unbootable`/`slot-retry-count` before concluding a restore
  "didn't work" — reflashing correct content alone is not enough once this trips. Full detail:
  memory `quest-slot-unbootable-recovery`.
- **`research-notes/74`: the leading root-cause hypothesis, not yet confirmed.** Unpacked and
  diff'd both boot images properly. DTB structure is equivalent (ruled out). **The ramdisk is a
  completely different architecture, not just a size gap**: stock (`~10.9MB`) is a monolithic,
  pre-two-stage-init ramdisk — real `init.rc`, `ueventd.rc`, one combined `sepolicy`,
  `fstab.monterey`, direct `bin -> /system/bin`-style symlinks, all living directly in the ramdisk
  (it effectively *is* the root filesystem). Ours (`~1.6MB`) is the modern AOSP two-stage-init
  shape — just a unified `init` binary + `first_stage_ramdisk/`. `BOARD_BUILD_SYSTEM_ROOT_IMAGE`
  (the old AOSP flag that used to select the monolithic style) is now `KATI_obsolete_var` on
  lineage-21 — there's no config toggle, the whole build system replaced the mechanism. Leading
  theory: this kernel (Android 8/9-era, already needs the LEGACYSAR patch) was never built for a
  two-stage-init handoff at all. `pstore` (`/sys/fs/pstore/`, real and configured,
  `CONFIG_PSTORE_RAM=y`) stayed **empty across every single failed attempt** — consistent with a
  failure early enough that first-stage init never gets far enough to write anything, i.e. an
  ABL-level or very-early-kernel rejection, not a later panic.

**Current action, in progress as of this checkpoint** (`research-notes/75`): sequenced plan, agreed
with the user — get **`lineage-18.1` (Android 11)** booting first, since it still natively supports
`BOARD_BUILD_SYSTEM_ROOT_IMAGE`, as a *proven reference*, before hand-building an equivalent
monolithic ramdisk for the lineage-21 tree (rather than reverse-engineering that shape blind). A
fully separate build environment was set up so the lineage-21 tree is untouched:
- `build/containers/lineageos18-build/` (Ubuntu 20.04, OpenJDK 11, `repo`, `git-lfs`).
- `work/lineageos18/` — a second source tree (gitignored). **`repo sync -c -j4 --no-clone-bundle`
  was running in the background when this checkpoint was written** — check progress after resume
  with `du -sh work/lineageos18` and `podman ps --filter ancestor=localhost/quest-lineageos18-build:latest`
  directly (the original background-task handle from the session that started it won't exist in a
  fresh session, but the container/sync itself runs independently of any particular chat session).
- `components/os/device-monterey-18/` — the new device tree, `BOARD_BUILD_SYSTEM_ROOT_IMAGE :=
  true` is the whole point of it. **Untested first draft** — expect the same class of
  config/discovery bugs `research-notes/68` found for the lineage-21 tree (lunch combo format,
  `PRODUCT_MAKEFILES` format, etc.) even though `add_lunch_combo` should actually be *correct* here
  (unlike lineage-21, where it was obsolete) since this is an older AOSP version.
- New Makefile targets in `components/os/Makefile`: `init-18`/`sync-18`/`lfs-pull-18`/
  `device-tree-link-18`/`kernel-prebuilt-18`/`lunch-18`. `container.mk`'s wrapper only supports one
  container image per Makefile, so these invoke `podman` directly against
  `quest-lineageos18-build` rather than fighting that.

**Next step on resume**: check `work/lineageos18` sync status; once done, `make -C components/os
lunch-18`, expect real config-fixing iteration (same as `research-notes/68`), then a real build,
then flash-and-boot-test using the `slot-unbootable`-aware recovery discipline above. If lineage-18.1
boots successfully, the actual monolithic ramdisk it produces becomes the reference for hand-building
an equivalent for the lineage-21 (Android 14) tree — that hand-build work has not started.

## Standing rules learned this session (in addition to everything in `research-notes/52`)

- **`fastboot getvar slot-unbootable:<slot>` before any recovery conclusion.** See above.
- **`podman stats`' aggregate CPU% can be a stale, misleading cumulative-average artifact.** Check
  individual process CPU *time* growth across two samples (not just the instantaneous "CPU %"
  field) to tell real progress from a stalled/zombie process tree. Hit this twice this session.
- **`podman logs` (no `-f`) can lag well behind real container activity** due to stdout buffering
  in the containerized process, not a sign of a hang — cross-check with `podman top`/`stats` before
  concluding something is stuck.
- **USB can stop enumerating entirely (`lsusb` shows nothing, `journalctl -k` shows nothing) while
  a device cycles through failed boots**, even with the cable genuinely connected the whole time —
  not necessarily a host-side or cable problem; it can resolve on its own within a couple of
  minutes.
- **`fastboot flash` can fail transiently mid-transfer** (`Error reading sparse file`) without the
  source file being corrupt — verify by re-hashing the source file, then just retry the flash.
- Always verify a backup against the **live, current** device state before trusting it for a
  risky flash, not just its own internal checksum (this project's own established habit, reinforced
  hard this session).
- When comparing two boot images to find a real behavioural difference, **use the actual AOSP
  `unpack_bootimg` tool** (`out/host/linux-x86/bin/unpack_bootimg --format=mkbootimg`) rather than
  hand-parsing header bytes — far less error-prone, and cheaply available once any LineageOS tree
  is synced.

## Also closed out this session, before step 5 started

- `research-notes/61`: controller tracker's ~20% outlier rate is one sustained ~7s "drift-lock"
  window (geometric ambiguity from no per-LED identity, unconfirmed), not scattered noise. Not
  fixed, mechanism understood.
- `research-notes/62`: motion-to-photon photon-attribution confirmed a software dead end on this
  device (`/dev/graphics/fb0` never written by the real compositor; `screencap` refused as a
  protected display). Needs a photodiode. Memory: `quest-motion-to-photon-blocked`.
- `research-notes/63`: distortion-mesh converter fidelity verified (sub-0.001px round-trip
  correct); absolute accuracy against real optics hits the identical wall as motion-to-photon.
- `research-notes/64`: sensors-HAL respawn root cause confirmed — Android's own `SensorService`
  (`system_server`) is a permanent client of `ISensors`, bundled into the same binary as the camera
  provider; any HAL death triggers an automatic, structural respawn. Not fixable; use the
  `ibfs_hook9.so` leech for any future sustained camera capture instead of stopping the HAL.

## Device state at checkpoint

Last verified fully working and hash-matched against known-good backups in `research-notes/73`
(after the `slot-unbootable` recovery). Not touched since — all work after that was source-tree/
build-only, no device interaction. `adb devices` shows nothing at the moment this checkpoint was
written, which is expected (idle/asleep/wifi-adb timeout), not a new issue — re-establish with
`adb connect <device-ip>:5555` or USB as usual on resume. Backups remain at
`backups/boot-monterey/new-boot_magisk30.7.img` (`boot_a`) and
`backups/1PASH9ACHD0215-2026-08-30T15-20-root/system_a.img` (`system_a`), both re-verified against
the live device multiple times this session.
