# components/controllers — replaces the controller half of vendor.oculus.hardware.sensors@1.0
# (IControllerProvider). See docs/controllers.md.
#
# sb_leech is the only piece that needs building: a raw /dev/syncboss_stream0 capture front-end
# (research-notes/30). ctl_decode.py decodes the capture on the host and needs no build step --
# `python3 ctl_decode.py <capture.bin>`. ctl_run.sh drives sb_leech on-device and needs no build
# either; it's pushed and run as-is.
CONTAINER_IMAGE := quest-ndk-r27c
include ../../build/mk/container.mk

ifeq ($(IN_CONTAINER),1)
NDK := /opt/android-ndk-r27c
CC  := $(shell ls $(NDK)/toolchains/llvm/prebuilt/*/bin/aarch64-linux-android29-clang | head -1)

.PHONY: all clean
all: sb_leech

sb_leech: sb_leech.c
	$(CC) -O2 -o $@ $<

clean:
	rm -f sb_leech
endif
