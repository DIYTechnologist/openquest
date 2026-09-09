# lineage_monterey.mk — product definition for the Quest 1 (monterey, msm8998).
#
# UNTESTED as of authoring (2026-09-09): the LineageOS 21 source sync this depends on
# (components/os/Makefile's `sync` target, work/lineageos/) was still running when this was
# written, so nothing here has been through a real build yet. Written from the actively-maintained
# device/motorola/msm8998-common and device/nokia/msm8998-common trees as structural reference
# (research-notes/65/67) plus this project's own confirmed facts (partition table, no vendor split,
# our own kernel build) -- expect real iteration once an actual `make` run can produce errors to
# fix. Deliberately minimal for the Phase 2 milestone (research-notes/18 step 5): boot to a shell,
# nothing else yet -- no audio/camera/bluetooth/telephony product packages included, since none of
# that is this milestone's goal and Quest 1 has no telephony hardware to speak of anyway.

$(call inherit-product, $(SRC_TARGET_DIR)/product/core_64_bit.mk)
$(call inherit-product, $(SRC_TARGET_DIR)/product/aosp_base.mk)

# Qualcomm CAF platform HALs (GPU/Adreno 540, codec2, etc.) -- this is the open, community-
# maintained Code Aurora Forum layer shared by every msm8998 device, not any phone's closed vendor
# blobs. Distinct from vendor/motorola or vendor/nokia's own proprietary trees, which are NOT
# inherited here -- monterey has none of that hardware.
$(call inherit-product, hardware/qcom-caf/common/common.mk)

$(call inherit-product-if-exists, device/oculus/monterey/device.mk)

PRODUCT_NAME := lineage_monterey
PRODUCT_DEVICE := monterey
PRODUCT_MANUFACTURER := Oculus
PRODUCT_BRAND := Oculus
PRODUCT_MODEL := Quest

# No GMS -- research-notes/16's "closed source we can port is fine; closed binaries are not"
# constraint rules it out, same reasoning that already scoped VrApi as a parked stretch goal there.
