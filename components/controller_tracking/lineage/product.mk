# SPDX-License-Identifier: MIT
# Opt in from the device product; the runtime remains disabled until configured.
PRODUCT_SOONG_NAMESPACES += external/openquest/components/controller_tracking
PRODUCT_PACKAGES += oq-controller-service oq-controller-replay oq-controller-live
