# BoardConfig.mk — monterey (Quest 1, msm8998/Snapdragon 835). UNTESTED, see lineage_monterey.mk's
# header. Adapted from device/motorola/msm8998-common and device/nokia/msm8998-common's
# BoardConfigCommon.mk (research-notes/67), diverging where monterey's actual hardware/partition
# table (research-notes/65) differs from a phone's.

TARGET_BOARD_PLATFORM := msm8998
TARGET_BOOTLOADER_BOARD_NAME := monterey

# CPU: msm8998/Snapdragon 835 is 4x Kryo 280 Gold (Cortex-A73-derived) + 4x Kryo 280 Silver
# (Cortex-A53-derived) -- same SoC as the reference devices, so their arch settings apply directly,
# not an adaptation.
TARGET_ARCH := arm64
TARGET_ARCH_VARIANT := armv8-a
TARGET_CPU_ABI := arm64-v8a
TARGET_CPU_VARIANT := generic
TARGET_CPU_VARIANT_RUNTIME := cortex-a73
TARGET_2ND_ARCH := arm
TARGET_2ND_ARCH_VARIANT := armv8-a
TARGET_2ND_CPU_ABI := armeabi-v7a
TARGET_2ND_CPU_ABI2 := armeabi
TARGET_2ND_CPU_VARIANT := generic
TARGET_2ND_CPU_VARIANT_RUNTIME := cortex-a73

BOARD_USES_QCOM_HARDWARE := true
TARGET_USES_ION := true
TARGET_USES_HWC2 := true
TARGET_USES_GRALLOC1 := true

# Kernel -- prebuilt from components/kernel, not built from source inside this tree. See device.mk's
# TARGET_PREBUILT_KERNEL comment for why (one source of truth for the kernel build, not two).
BOARD_KERNEL_IMAGE_NAME := Image.gz-dtb
BOARD_KERNEL_BASE := 0x00000000
BOARD_KERNEL_PAGESIZE := 4096
# androidboot.hardware=monterey, not the generic "qcom" reference-tree value: unpacked the LIVE
# stock boot_a with unpack_bootimg for a direct comparison (research-notes/73) and found this is
# the single most device-specific divergence in the whole cmdline. androidboot.hardware selects
# which init.<hardware>.rc gets parsed, and this device's bootloader carries custom OEM extensions
# (`oculus-ext-version:2`, research-notes/65) -- plausible this matters earlier than just init.
# Still deliberately minimal otherwise: no androidboot.veritymode (verity/AVB both off), no
# telephony/RIL flags -- isolating one variable at a time rather than copying stock's cmdline
# wholesale, same discipline research-notes/21 already established for this device.
BOARD_KERNEL_CMDLINE := androidboot.hardware=monterey androidboot.configfs=true loop.max_part=7

# Partitions -- from research-notes/65's `fastboot getvar all` capture on the real device, not
# guessed. boot_a/boot_b and system_a/system_b are A/B slotted; no vendor_a/vendor_b because there
# is no separate vendor partition at all (research-notes/16/65) -- vendor content lives inside
# system.img, hence TARGET_COPY_OUT_VENDOR below rather than the reference devices' `vendor`.
# Decimal, not the 0x... form fastboot printed: build_image.py's verity_utils.py parses this field
# with int(s, base=10), which rejects a "0x" prefix outright -- found 92% into a build
# (research-notes/68) via a plain ValueError, not a hex-vs-decimal issue anywhere else in the
# toolchain (BOARD_KERNEL_BASE stays hex; nothing downstream of it hit this same parser).
BOARD_BOOTIMAGE_PARTITION_SIZE := 67108864    # 0x4000000
BOARD_SYSTEMIMAGE_PARTITION_SIZE := 2684354560  # 0xA0000000
BOARD_FLASH_BLOCK_SIZE := 0x40000
TARGET_USERIMAGES_USE_EXT4 := true
TARGET_COPY_OUT_VENDOR := system/vendor
AB_OTA_UPDATER := true
AB_OTA_PARTITIONS += boot system

# Verified Boot -- OFF. This device is VB1 with no AVB2/vbmeta mechanism at all
# (research-notes/32), and the bootloader is now genuinely unlocked (research-notes/65/66), so there
# is nothing for AVB to attach to and no reason to fight it on a from-scratch bring-up. Matches the
# reference devices' own choice (`BOARD_AVB_ENABLE := false`), for a different but compatible
# reason (they have real AVB2 hardware and choose not to use it; we don't have it at all).
BOARD_AVB_ENABLE := false

# Treble/VNDK -- PRODUCT_FULL_TREBLE_OVERRIDE deliberately NOT set (unlike the reference devices).
# Full Treble conventionally implies a real vendor partition; monterey has vendor-inside-system
# instead (same as the STOCK OS's own layout, research-notes/16). Leaving this unset for Phase 2's
# "boot to a shell" milestone rather than asserting a Treble configuration this device's partition
# table doesn't actually support -- open item to revisit once a real build surfaces whether it's
# needed for something specific.
BOARD_VNDK_VERSION := current

# Recovery -- not attempting a recovery image in Phase 2's scope.
TARGET_NO_RECOVERY := true
BOARD_USES_RECOVERY_AS_BOOT := false

TARGET_RECOVERY_FSTAB := device/oculus/monterey/fstab.monterey
