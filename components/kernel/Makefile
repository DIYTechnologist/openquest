# components/kernel — the monterey kernel build (research-notes/21, research-notes/32). Not a
# userspace service, but a real replacement binary (Image.gz-dtb) and, for now, the vehicle for the
# camera-debug instrumentation components/camera's bring-up used. See docs/kernel.md.
#
#   make            baseline -- config byte-identical to the device except the one documented
#                    deviation in CONFIG_DEVIATIONS.md
#   make instrument  adds CONFIG_MSMB_CAMERA_DEBUG=y + CONFIG_DYNAMIC_DEBUG=y
#
# Needs work/oculus-kernel (Meta's published GPLv2 source, separately obtained -- research-notes/21)
# present in the repo checkout; it's mounted into the container, not baked into the image.
CONTAINER_IMAGE := quest-kernel-toolchain
include ../../build/mk/container.mk

ifeq ($(IN_CONTAINER),1)
.PHONY: all instrument clean
all:
	sh build.sh
instrument:
	sh build.sh instrument
clean:
	rm -rf build
endif
