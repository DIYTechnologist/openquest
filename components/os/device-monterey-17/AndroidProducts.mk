# device/oculus/monterey -- lineage-17.1 (Android 10) bring-up reference build (research-notes/78).
# Purpose: research-notes/78 found lineage-18.1 (Android 11) has two-stage init baked in
# unconditionally regardless of BOARD_BUILD_SYSTEM_ROOT_IMAGE, so it can't produce the stock
# device's real single-stage/monolithic ramdisk shape (research-notes/74) at all. The stock device
# is confirmed running Android 10 itself (research-notes/03/17), so lineage-17.1 is the matching
# branch to try next, both for version-matching and because it's old enough two-stage init wasn't
# yet unconditional. A reference/validation build, not the end-goal OS -- see
# components/os/device-monterey/ for that.
#
# UNTESTED first draft, adapted from the (now-working, research-notes/76/77/78) device-monterey-18
# tree. Not yet verified against the real lineage-17.1 source. Expect real iteration -- in
# particular, whether hardware/qcom-caf/common/common.mk exists at THIS branch's tip is unknown
# (research-notes/78 found it doesn't exist at lineage-18.1's tip despite existing at lineage-21's,
# the reverse of the obvious guess) -- check directly once synced, don't assume either way.

PRODUCT_MAKEFILES := \
    lineage_monterey:$(LOCAL_DIR)/lineage_monterey.mk

COMMON_LUNCH_CHOICES := \
    lineage_monterey-userdebug \
    lineage_monterey-user
