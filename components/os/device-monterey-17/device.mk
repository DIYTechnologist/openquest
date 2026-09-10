# device.mk -- lineage-17.1 (Android 10) bring-up reference build. UNTESTED first draft.

LOCAL_PATH := device/oculus/monterey

# 29 = Android 10, matching the stock device's own confirmed version (research-notes/03/17), not
# 30 (Android 11) as the lineage-18.1 tree used.
PRODUCT_SHIPPING_API_LEVEL := 29
TARGET_SUPPORTS_64_BIT_APPS := true

# Same kernel prebuilt as the other two trees -- one source of truth
# (components/kernel + components/os/patch_legacysar_kernel.py), not a second build.
TARGET_PREBUILT_KERNEL := device/oculus/monterey/prebuilt/Image.gz-dtb

PRODUCT_COPY_FILES += \
    $(LOCAL_PATH)/fstab.monterey:$(TARGET_COPY_OUT_VENDOR)/etc/fstab.monterey \
    $(LOCAL_PATH)/fstab.monterey:root/fstab.monterey

# Deliberately minimal -- this build's only job is proving the real single-stage/monolithic-ramdisk
# boot path works (research-notes/74/78), not being a usable OS. No telephony/audio/camera packages.
