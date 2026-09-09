# BoardConfig.mk -- lineage-18.1 (Android 11) bring-up reference build. UNTESTED first draft.
# Adapted from components/os/device-monterey/BoardConfig.mk (the lineage-21 tree), with the one
# change that's the entire point of this build (research-notes/74):

# THE key difference from the lineage-21 tree: this AOSP version still supports the OLD monolithic
# (system-as-root-in-ramdisk) boot style this kernel's early boot actually expects -- confirmed
# obsolete (KATI_obsolete_var) on lineage-21, research-notes/72. Real ramdisk content comparison
# (research-notes/74) showed the live stock ramdisk IS built this way: init.rc/sepolicy/fstab
# directly at the ramdisk root, not the modern two-stage-init shape our lineage-21 build produced.
BOARD_BUILD_SYSTEM_ROOT_IMAGE := true

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
