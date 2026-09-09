# `components/os`

The OS swap itself (`research-notes/18` step 5) — replacing the stock Quest 1 OS with LineageOS 21
(Android 14), the payoff step everything else in this project has been building toward. Not a
single binary like the other components; a device tree (`device/oculus/monterey/`) plus the
LineageOS source it builds against.

## Status

**Just started, 2026-09-09** (`research-notes/65`-`67`). Prerequisite work done: the bootloader is
confirmed genuinely unlocked (QuestStack/CVE-2021-1931, `fastboot getvar unlocked:yes`,
`research-notes/65`) and the unlocked flash/boot loop is validated clean (`research-notes/66`). A
first `device/oculus/monterey/` draft exists (`research-notes/67`) but **has not been through a
real build** — the LineageOS source sync it depends on was still running when it was written.
Expect real iteration once a build attempt can actually produce errors to fix.

## What it does (once real)

- No `monterey` device tree exists anywhere upstream — msm8998 (Snapdragon 835) itself is still
  actively supported by LineageOS (`lineage-21` work exists for several OEMs), just not this
  device, so this is a from-scratch tree using the actively-maintained
  `device/motorola/msm8998-common` and `device/nokia/msm8998-common` trees as structural reference
  only, not something forked.
- Consumes `components/kernel`'s already-working kernel build as a prebuilt
  (`TARGET_PREBUILT_KERNEL`), not a second from-source kernel build inside the LineageOS tree — one
  source of truth.
- No separate vendor partition (`research-notes/16`/`65`: `/vendor` is a symlink into `/system`
  even on the stock OS) — `TARGET_COPY_OUT_VENDOR := system/vendor`, diverging from the reference
  phones, which have a real vendor partition.
- AVB disabled (`BOARD_AVB_ENABLE := false`) — this device has no AVB2/vbmeta mechanism at all
  (`research-notes/32`), and the bootloader is genuinely unlocked, so there's nothing for AVB to
  attach to.
- Deliberately minimal for the first milestone: no audio/camera/bluetooth/telephony product
  packages yet. This component's own `components/{camera,controllers,tracking}` binaries becoming
  the real vendor HAL layer under this new OS's init (rather than binder-injecting into Meta's
  still-running stock processes, which is how they work today) is later, separate work.

## Build & run

```
make -C components/os init             # repo init against LineageOS 21 (lineage-21.0)
make -C components/os sync             # repo sync -- large, hours, run in the background
make -C components/kernel               # build the kernel components/os will consume as a prebuilt
make -C components/os lunch             # device-tree-link + kernel-prebuilt + lunch + build
```

Needs the `quest-lineageos-build` container (Ubuntu 20.04, OpenJDK 17, `repo` — `build/containers/
lineageos-build/`; Ubuntu 20.04 specifically because AOSP's documented host package list was
dropped from 22.04's repos, the same reasoning as the kernel container's pinned GCC 4.9) and real
disk space: a LineageOS source sync plus build output needs 250-400GB, checked explicitly before
starting rather than assumed available.

## Known limits

- Untested end to end — see Status above.
- `PRODUCT_FULL_TREBLE_OVERRIDE` deliberately left unset rather than asserted true (unlike the
  reference devices), since full Treble conventionally assumes a real vendor partition this device
  doesn't have. Open item, not resolved.
- `device.mk` inherits `full_base_telephony.mk` for now (RIL/telephony scaffolding this device may
  have no hardware for, despite `modem_a`/`modem_b` existing in the partition table — possibly
  pin-compatible board legacy, unconfirmed) — flagged as a likely thing to change once a build
  attempt shows whether it actually matters.
- Flashing `system_a` for the first real build attempt is real, meaningfully-more-than-Phase-1 risk
  (a from-scratch OS, not a known-good re-flash) — needs fresh-verified `boot_a`/`persist` backups
  and explicit confirmation before that flash, same discipline as `research-notes/66`.
