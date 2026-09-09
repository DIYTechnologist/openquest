# Step 5 Phase 2 kickoff: build container, LineageOS 21 sync started, first device tree draft — 2026-09-09

Continuation of `research-notes/65`/`66` on the same day. Phase 2's scope, per the approved plan:
pick a reference and Android/LineageOS version, and get a `device/oculus/monterey/` skeleton to the
point of a first buildable attempt. Version target decided with the user: **lineage-21.0
(Android 14)** — better Vulkan/OpenXR-adjacent platform support for Monado/ALVR later, and msm8998
turns out not to be an abandoned platform: `LineageOS/android_device_motorola_msm8998-common` and
`android_device_nokia_msm8998-common` both have active `lineage-21` work, so old-kernel-plus-modern-
userspace is a proven pattern on this exact SoC already, not something monterey would pioneer.

## Disk space was a hard blocker, resolved by the user

The host had only 16GB free (root filesystem at 96%) against a 250-400GB requirement for a
LineageOS source sync plus build output. Flagged directly rather than attempting it and risking
filling the host's disk. The user expanded available space to 617GB free before continuing.

## Build container: `quest-lineageos-build`

Same pattern as every other container in this project (`build/containers/`, `build/mk/container.mk`)
— a pinned, disposable toolchain rather than installing a huge, version-sensitive AOSP build
environment on the host. Notably **Ubuntu 20.04, not 22.04** (unlike the other three containers):
AOSP/LineageOS's documented host package list (`lib32ncurses5-dev`, `libncurses5`,
`lib32readline-dev`) was dropped from Ubuntu's repos in 22.04 — same "the project needs the exact
old thing" reasoning as the kernel container's pinned GCC 4.9, just for host build deps instead of a
cross-compiler. OpenJDK 17 (Android 14 AOSP requires it specifically; the host's own JDK is 26, far
too new) and Google's `repo` launcher are both baked in. Builds clean.

## `components/os/`: the device tree lives here, the OS source doesn't

New component directory, structured like `components/kernel`'s relationship to
`work/oculus-kernel`: the actual LineageOS source (`work/lineageos/`, gitignored, 100GB+) isn't
authored by us and isn't tracked; what's tracked is `components/os/device-monterey/` (our own
device tree) plus a `Makefile` with `init`/`sync`/`device-tree-link`/`kernel-prebuilt`/`lunch`
targets, all container-wrapped via the same `container.mk` every other component uses.

`repo init -b lineage-21.0` against `LineageOS/android.git` succeeded immediately. `repo sync -c
-j4 --no-clone-bundle` was launched in the background (`-c`: current branch only, LineageOS's own
recommendation for device bring-up rather than every branch's full history; `--no-clone-bundle`:
this project has hit CDN/network flakiness on large transfers before, `research-notes/32`'s UFS
wedge investigation, so preferring the slower but more resumable plain-fetch path was deliberate,
not default). **Still running as of this note** — 25GB and climbing steadily after the first ~10
minutes, no errors.

## First device tree draft — real, but explicitly UNTESTED

Wrote `AndroidProducts.mk`, `lineage_monterey.mk`, `device.mk`, `BoardConfig.mk`, `fstab.monterey`,
`vendorsetup.sh`. Informed directly by the motorola/nokia `msm8998-common` `BoardConfigCommon.mk`/
`common.mk`/`fstab.qcom` files (cloned to `/tmp/msm8998-ref/` for reference, not vendored into this
repo), diverging where monterey's actual, measured hardware differs from a phone:

- **`TARGET_COPY_OUT_VENDOR := system/vendor`**, not `vendor` — monterey has no separate vendor
  partition (`research-notes/16`/`65`, confirmed independently from both the stock OS's property
  side and this session's own `fastboot getvar all`), so vendor content has to live inside
  `system.img` the same way it already does on the stock OS, not on its own partition like the
  reference phones. `PRODUCT_FULL_TREBLE_OVERRIDE` deliberately left **unset** rather than copying
  the reference devices' `true` — full Treble conventionally assumes a real vendor partition, and
  asserting it against a partition table that doesn't have one felt like exactly the kind of
  unverified assumption this project tries not to make. Flagged as open, not resolved either way.
- **`TARGET_PREBUILT_KERNEL`**, not `TARGET_KERNEL_SOURCE` — the reference devices build
  `kernel/<oem>/msm8998` as part of the AOSP tree; monterey's kernel is `components/kernel`, already
  independently buildable with its own pinned, byte-identical-to-the-device toolchain
  (`research-notes/21`). Re-deriving that inside the LineageOS build tree too would duplicate it and
  risk drift, so `components/os`'s `kernel-prebuilt` target just copies `components/kernel`'s
  already-built `Image.gz-dtb` in — one source of truth for the kernel, consumed as a prebuilt here.
- **`BOARD_AVB_ENABLE := false`, no `avb=` fstab flags** — matches the reference devices' own
  choice, for a different but compatible reason: they have real AVB2 hardware and choose not to use
  it, monterey has no AVB2/vbmeta mechanism at all (`research-notes/32`) and a genuinely unlocked
  bootloader (`research-notes/65`), so there's nothing to attach AVB to and no reason to fight it on
  a from-scratch bring-up.
- **Partition sizes taken from this session's own `fastboot getvar all`** (`research-notes/65`),
  not guessed or copied from the reference: `boot_a`/`boot_b` 0x4000000, `system_a`/`system_b`
  0xA0000000.
- **Deliberately minimal**: no audio/camera/bluetooth/telephony `PRODUCT_PACKAGES`, no `vendor/
  qcom-caf`-style proprietary blob inherits beyond the open `hardware/qcom-caf/common/common.mk`
  (the community-maintained, open Qualcomm platform HAL layer — GPU/codec2/etc. — distinct from any
  phone's closed vendor blobs, which are NOT inherited since monterey has none of that hardware).
  Matches the plan's own explicitly small Phase 2 scope: boot to a shell, nothing else yet.

**None of this has been through a real build.** The sync it depends on was still running when this
was written. Every file says so in its own header comment, not just here — expect real iteration
once `make bootimage systemimage` can actually produce errors to fix, especially around
`full_base_telephony.mk` (pulls in RIL scaffolding for hardware this device may not have, flagged
inline in `device.mk`) and the unset Treble override.

## What's next

Once the sync finishes: `make -C components/os lunch` (device-tree-link + kernel-prebuilt +
lunch + build, per the Makefile) is the first real test. Expect it not to work first try — this is
a from-scratch device tree against a platform with no prior monterey port anywhere, and the honest
bar per the approved plan is "reaches `adb shell`," not "works cleanly on the first attempt."
