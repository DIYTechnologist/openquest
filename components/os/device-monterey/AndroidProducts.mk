# device/oculus/monterey — Quest 1 (msm8998). No such tree exists anywhere upstream
# (research-notes/65's search); built from scratch against LineageOS 21 (Android 14), using the
# actively-maintained device/motorola/msm8998-common and device/nokia/msm8998-common trees as
# structural reference only -- not forked, since both assume a phone (modem/RIL, touchscreen,
# separate vendor partition) monterey doesn't have.

# <product_name>:<path> -- a bare path isn't enough on this AOSP version (checked against a real
# in-tree example, device/google/cuttlefish/AndroidProducts.mk, after the bare-path form silently
# failed to resolve with "Cannot locate config makefile for product 'lineage_monterey'").
PRODUCT_MAKEFILES := \
    lineage_monterey:$(LOCAL_DIR)/lineage_monterey.mk

COMMON_LUNCH_CHOICES := \
    lineage_monterey-ap2a-userdebug \
    lineage_monterey-ap2a-user
