# device.mk -- lineage-18.1 bring-up reference build. UNTESTED first draft.

LOCAL_PATH := device/oculus/monterey

PRODUCT_SHIPPING_API_LEVEL := 30
TARGET_SUPPORTS_64_BIT_APPS := true

# Same kernel prebuilt as the Android 14 tree -- one source of truth
# (components/kernel + components/os/patch_legacysar_kernel.py), not a second build.
TARGET_PREBUILT_KERNEL := device/oculus/monterey/prebuilt/Image.gz-dtb

PRODUCT_COPY_FILES += \
    $(LOCAL_PATH)/fstab.monterey:$(TARGET_COPY_OUT_VENDOR)/etc/fstab.monterey \
    $(LOCAL_PATH)/fstab.monterey:root/fstab.monterey

# Deliberately minimal -- this build's only job is proving the monolithic-ramdisk boot path works
# at all (research-notes/74), not being a usable OS. No telephony/audio/camera packages.
