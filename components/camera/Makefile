# components/camera — replaces libqcameraoculushal.so, libqcameradriver.so, libsyncboss.so
# (camera+MCU control) and cameramuxmodeservice. See docs/camera.md.
#
# Usage: make            (from here, or `make -C components/camera` from the repo root)
#        make clean
#
# KSRC may be overridden; defaults to the published kernel tree at work/oculus-kernel
# (research-notes/21, research-notes/19).
CONTAINER_IMAGE := quest-ndk-r27c
include ../../build/mk/container.mk

ifeq ($(IN_CONTAINER),1)
NDK  ?= /opt/android-ndk-r27c
CC   := $(shell ls $(NDK)/toolchains/llvm/prebuilt/*/bin/aarch64-linux-android29-clang | head -1)
KSRC ?= $(REPO_ROOT)/work/oculus-kernel
KINC := .kinc

.PHONY: all clean
all: cam_kernel

# msm_ioctl_table.h isn't needed by cam_kernel.c itself -- it's here because it's generated from
# the same published headers this component builds against, and tools/cam_kernel/ioctl_trace.c
# (the RE tool that produced the original reference trace this component reimplements) includes it
# by path. Regenerate with `sh src/gen_ioctl_table.sh $(KSRC)` if the kernel tree changes; it is
# committed since it rarely does and is small/readable.

# Stage ONLY the media/ headers the NDK lacks. Putting $(KSRC)/include/uapi on the include path
# wholesale makes bionic's own <linux/types.h> resolve into the kernel tree and redefine
# struct sigaction (research-notes/19). __user is a sparse annotation, invalid outside the kernel
# build. The SGRBG14 redefinition fixup matches a real duplicate macro in msmb_isp.h.
$(KINC)/.stamp:
	rm -rf $(KINC) && mkdir -p $(KINC)/media $(KINC)/linux
	cp $(KSRC)/include/uapi/media/*.h $(KINC)/media/
	cp $(KSRC)/include/uapi/linux/media.h $(KINC)/linux/
	cp $(KSRC)/drivers/staging/android/uapi/ion.h $(KINC)/linux/
	sed -i 's/__user //g; s/ __user//g' $(KINC)/media/*.h $(KINC)/linux/*.h
	sed -i 's/#define V4L2_PIX_FMT_SGRBG14/#undef V4L2_PIX_FMT_SGRBG14\n#define V4L2_PIX_FMT_SGRBG14/' \
	  $(KINC)/media/msmb_isp.h
	touch $@

cam_kernel: src/cam_kernel.c $(KINC)/.stamp
	$(CC) -O2 -std=c11 -o $@ src/cam_kernel.c -I. -I$(KINC)

clean:
	rm -rf cam_kernel $(KINC)
endif
