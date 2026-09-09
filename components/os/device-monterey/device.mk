# device.mk — monterey device-specific product config. See lineage_monterey.mk's header for the
# "untested, minimal-on-purpose" context; same applies here.

LOCAL_PATH := device/oculus/monterey

PRODUCT_SHIPPING_API_LEVEL := 34
TARGET_SUPPORTS_64_BIT_APPS := true

# Kernel: TARGET_PREBUILT_KERNEL, not TARGET_KERNEL_SOURCE. Unlike the motorola/nokia reference
# trees (which build kernel/<oem>/msm8998 as part of the AOSP tree), monterey's kernel is
# components/kernel -- its own independently-buildable component with its own pinned AOSP GCC 4.9
# container toolchain (byte-identical compiler string to the device's own /proc/version,
# research-notes/21). Re-deriving that toolchain setup inside the LineageOS build tree too would
# duplicate it and risk drift; consuming components/kernel's build output as a prebuilt keeps one
# source of truth. Path below assumes `make -C components/kernel` has been run and its output
# copied/symlinked in -- not yet wired up as of this file's authoring, real TODO once the source
# sync finishes and a first build is attempted.
TARGET_PREBUILT_KERNEL := device/oculus/monterey/prebuilt/Image.gz-dtb

PRODUCT_COPY_FILES += \
    $(LOCAL_PATH)/fstab.monterey:$(TARGET_COPY_OUT_VENDOR)/etc/fstab.monterey \
    $(LOCAL_PATH)/fstab.monterey:$(TARGET_COPY_OUT_RAMDISK)/first_stage_ramdisk/fstab.monterey

PRODUCT_PACKAGES += \
    android.hardware.boot@1.0-impl \
    android.hardware.boot@1.0-service

$(call inherit-product, $(SRC_TARGET_DIR)/product/full_base_telephony.mk)
# ^ TODO once building: full_base_telephony.mk pulls in RIL/telephony scaffolding this device has
# no hardware for (no modem, despite modem_a/modem_b partitions existing in the table -- probably
# a pin-compatible board legacy, research-notes/65). Likely needs swapping for a no-telephony base
# product once that's confirmed to matter; kept for now since AOSP's telephony-free base products
# have historically been less reliable to build against than commenting out RIL packages afterward.
