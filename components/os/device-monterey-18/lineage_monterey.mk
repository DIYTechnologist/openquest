# lineage_monterey.mk -- lineage-18.1 (Android 11) bring-up reference build. UNTESTED first draft,
# see AndroidProducts.mk's header for context/purpose.

$(call inherit-product, $(SRC_TARGET_DIR)/product/core_64_bit.mk)
# core_64_bit.mk's own comment says it must come before the chain that leads to core_minimal.mk --
# without this, PRODUCT_PACKAGES stays empty (no init/sepolicy/toybox/etc get declared), and the
# classic ramdisk this build exists to produce (research-notes/74/78) packs as a technically-real
# but functionally-empty cpio (256 bytes uncompressed, TRAILER record only, no init.rc at all).
# Matches the lineage-21 tree's equivalent inherit (device-monterey/lineage_monterey.mk).
$(call inherit-product, $(SRC_TARGET_DIR)/product/aosp_base.mk)

# NOT inheriting hardware/qcom-caf/common/common.mk here, unlike the lineage-21 tree's equivalent
# file: checked against the real synced lineage-18.1 source (research-notes/76) and, surprisingly,
# common.mk does not exist at this branch's tip -- that repo only ships os_pickup.mk/fwk-detect at
# lineage-18.1 (it gained common.mk later, by lineage-21.0). Since this build's only job is proving
# the monolithic-ramdisk boot path (research-notes/74), not any HAL functionality, dropping it
# rather than chasing a path that doesn't exist on this branch.
$(call inherit-product-if-exists, device/oculus/monterey/device.mk)

PRODUCT_NAME := lineage_monterey
PRODUCT_DEVICE := monterey
PRODUCT_MANUFACTURER := Oculus
PRODUCT_BRAND := Oculus
PRODUCT_MODEL := Quest

# No GMS -- same reasoning as the lineage-21 tree (research-notes/16's "closed source we can port
# is fine; closed binaries are not").
