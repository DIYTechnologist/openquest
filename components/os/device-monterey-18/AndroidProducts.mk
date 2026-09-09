# device/oculus/monterey -- lineage-18.1 (Android 11) bring-up reference build (research-notes/74).
# Purpose: validate this kernel actually boots with the OLD monolithic ramdisk style
# (BOARD_BUILD_SYSTEM_ROOT_IMAGE, still supported on this AOSP version -- obsolete on lineage-21,
# research-notes/72) before hand-building an equivalent for the Android 14 tree. A reference/
# validation build, not the end-goal OS -- see components/os/device-monterey/ for that.
#
# UNTESTED first draft, same discipline as research-notes/67's original device-monterey draft:
# adapted from Android 10/11-era AOSP conventions, not yet verified against the real lineage-18.1
# source (which wasn't synced yet when this was written). Expect real iteration.

PRODUCT_MAKEFILES := \
    lineage_monterey:$(LOCAL_DIR)/lineage_monterey.mk

COMMON_LUNCH_CHOICES := \
    lineage_monterey-userdebug \
    lineage_monterey-user
