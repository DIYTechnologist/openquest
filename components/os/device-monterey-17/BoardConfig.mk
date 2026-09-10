# BoardConfig.mk -- lineage-17.1 (Android 10) bring-up reference build. UNTESTED first draft.
# Adapted from components/os/device-monterey-18/BoardConfig.mk, itself adapted from
# components/os/device-monterey/BoardConfig.mk (the lineage-21 tree).

# BOARD_BUILD_SYSTEM_ROOT_IMAGE deliberately left UNSET here, matching the lineage-18.1 tree's
# working config (research-notes/78) -- true means system-as-root there (a near-empty ramdisk,
# opposite of research-notes/74's original assumption). BUT research-notes/78 also found that even
# with this unset, lineage-18.1 (Android 11) still produces only a two-stage-init first-stage
# ramdisk (bare `init` binary + mountpoints, no init.rc/sepolicy/symlinks) -- AOSP's own Makefile
# there calls that shape "the first stage ramdisk" unconditionally. WHETHER lineage-17.1 (Android
# 10, one branch further back) can still produce the stock device's real single-stage/monolithic
# shape at all is the entire open question this build exists to answer -- check the actual built
# ramdisk's content directly (unpack_bootimg, look for a real init.rc at ramdisk root) once this
# builds, don't assume unset is sufficient just because it was the right call on lineage-18.1.

TARGET_BOARD_PLATFORM := msm8998
TARGET_BOOTLOADER_BOARD_NAME := monterey

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

BOARD_KERNEL_IMAGE_NAME := Image.gz-dtb
BOARD_KERNEL_BASE := 0x00000000
BOARD_KERNEL_PAGESIZE := 4096
# androidboot.hardware=monterey: confirmed via direct unpack_bootimg comparison against the live
# stock boot_a (research-notes/73) -- correct regardless of the ramdisk-architecture fix, kept.
BOARD_KERNEL_CMDLINE := androidboot.hardware=monterey androidboot.configfs=true loop.max_part=7

# Partitions -- same real, measured values as the lineage-21 tree (research-notes/65/68), decimal
# for the same reason (build_image.py's int(s, base=10)).
BOARD_BOOTIMAGE_PARTITION_SIZE := 67108864    # 0x4000000
BOARD_SYSTEMIMAGE_PARTITION_SIZE := 2684354560  # 0xA0000000
BOARD_FLASH_BLOCK_SIZE := 0x40000
TARGET_USERIMAGES_USE_EXT4 := true
TARGET_COPY_OUT_VENDOR := system/vendor
AB_OTA_UPDATER := true
AB_OTA_PARTITIONS += boot system

# Verified Boot -- OFF, same reasoning as the lineage-21 tree (VB1, no AVB2/vbmeta, genuinely
# unlocked bootloader, research-notes/32/65).
BOARD_AVB_ENABLE := false

BOARD_VNDK_VERSION := current

TARGET_NO_RECOVERY := true
BOARD_USES_RECOVERY_AS_BOOT := false

TARGET_RECOVERY_FSTAB := device/oculus/monterey/fstab.monterey
