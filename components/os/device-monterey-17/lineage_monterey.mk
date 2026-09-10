# lineage_monterey.mk -- lineage-17.1 (Android 10) bring-up reference build. UNTESTED first draft,
# see AndroidProducts.mk's header for context/purpose.

$(call inherit-product, $(SRC_TARGET_DIR)/product/core_64_bit.mk)
# core_64_bit.mk's own comment says it must come before the chain that leads to core_minimal.mk --
# without this, PRODUCT_PACKAGES stays empty (no init/sepolicy/toybox/etc get declared), and the
# ramdisk this build exists to produce (research-notes/74/78) packs as a technically-real but
# functionally-empty cpio. Learned the hard way on the lineage-18.1 tree (research-notes/78 bug 4)
# -- carrying the fix forward rather than re-discovering it.
$(call inherit-product, $(SRC_TARGET_DIR)/product/aosp_base.mk)

# TODO once synced: check directly (don't assume from the lineage-18.1 result either way) whether
# hardware/qcom-caf/common/common.mk exists at lineage-17.1's tip. research-notes/78 found it does
# NOT exist at lineage-18.1's tip despite existing at lineage-21's -- the reverse of the obvious
# "older = simpler/legacy" guess -- so lineage-17.1 (one branch further back) could go either way.
# Currently omitted (matching the lineage-18.1 tree's working config); add back in if a real build
# error says otherwise.
$(call inherit-product-if-exists, device/oculus/monterey/device.mk)

PRODUCT_NAME := lineage_monterey
PRODUCT_DEVICE := monterey
PRODUCT_MANUFACTURER := Oculus
PRODUCT_BRAND := Oculus
PRODUCT_MODEL := Quest

# No GMS -- same reasoning as the lineage-21 tree (research-notes/16's "closed source we can port
# is fine; closed binaries are not").
