# lineage_monterey.mk -- lineage-18.1 (Android 11) bring-up reference build. UNTESTED first draft,
# see AndroidProducts.mk's header for context/purpose.

$(call inherit-product, $(SRC_TARGET_DIR)/product/core_64_bit.mk)

# Qualcomm CAF platform HALs -- same reasoning as device-monterey/lineage_monterey.mk (the open,
# community CAF layer shared by every msm8998 device, not any phone's closed vendor blobs). Path
# may need adjusting once the real lineage-18.1 source is available to check against.
$(call inherit-product, hardware/qcom-caf/common/common.mk)

$(call inherit-product-if-exists, device/oculus/monterey/device.mk)

PRODUCT_NAME := lineage_monterey
PRODUCT_DEVICE := monterey
PRODUCT_MANUFACTURER := Oculus
PRODUCT_BRAND := Oculus
PRODUCT_MODEL := Quest

# No GMS -- same reasoning as the lineage-21 tree (research-notes/16's "closed source we can port
# is fine; closed binaries are not").
