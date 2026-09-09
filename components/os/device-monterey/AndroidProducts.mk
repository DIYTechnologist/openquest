# device/oculus/monterey — Quest 1 (msm8998). No such tree exists anywhere upstream
# (research-notes/65's search); built from scratch against LineageOS 21 (Android 14), using the
# actively-maintained device/motorola/msm8998-common and device/nokia/msm8998-common trees as
# structural reference only -- not forked, since both assume a phone (modem/RIL, touchscreen,
# separate vendor partition) monterey doesn't have.

PRODUCT_MAKEFILES := \
    $(LOCAL_DIR)/lineage_monterey.mk

COMMON_LUNCH_CHOICES := \
    lineage_monterey-userdebug \
    lineage_monterey-user
