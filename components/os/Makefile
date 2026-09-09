# components/os — the from-scratch device/oculus/monterey tree + the LineageOS 21 (Android 14)
# source it builds against (research-notes/18 step 5, research-notes/65/66).
#
# Unlike the other components, the actual OS source (work/lineageos/, gitignored, ~100GB+) isn't
# something we author -- what's tracked here is our own device/oculus/monterey tree (symlinked into
# work/lineageos at sync/build time) plus the driver targets. Same "big external source tree lives
# in work/, only our own small changes are tracked" pattern as components/kernel's work/oculus-kernel.
CONTAINER_IMAGE := quest-lineageos-build
include ../../build/mk/container.mk

LINEAGEOS_DIR := $(REPO_ROOT)/work/lineageos
# repo/git need an identity and a writable HOME; the container's filesystem is ephemeral per
# invocation (only REPO_ROOT is mounted), so both are set per-run via env vars rather than relying
# on a persistent git config or a home directory that exists inside the image.
BUILD_ENV := HOME=$(LINEAGEOS_DIR)/.home \
             GIT_AUTHOR_NAME="Quest OS Build" GIT_AUTHOR_EMAIL="quest-build@localhost" \
             GIT_COMMITTER_NAME="Quest OS Build" GIT_COMMITTER_EMAIL="quest-build@localhost"

ifeq ($(IN_CONTAINER),1)
.PHONY: init sync device-tree-link kernel-prebuilt lunch

init:
	mkdir -p $(LINEAGEOS_DIR) $(LINEAGEOS_DIR)/.home
	cd $(LINEAGEOS_DIR) && $(BUILD_ENV) repo init -u https://github.com/LineageOS/android.git -b lineage-21.0

# -c: current branch only (LineageOS wiki's own recommendation for device bring-up -- full history
# of every branch is not needed and roughly triples the download). --no-clone-bundle: the bundle
# CDN has been unreliable for large syncs before elsewhere in this project (UFS/wifi flakiness,
# research-notes/32) -- prefer the slower but more resumable plain fetch path.
sync:
	cd $(LINEAGEOS_DIR) && $(BUILD_ENV) repo sync -c -j4 --no-clone-bundle --force-sync

device-tree-link:
	mkdir -p $(LINEAGEOS_DIR)/device/oculus
	ln -sfn $(REPO_ROOT)/components/os/device-monterey $(LINEAGEOS_DIR)/device/oculus/monterey

# One source of truth for the kernel build (components/kernel, its own container/toolchain) --
# this just copies the already-built output in, matching device.mk's TARGET_PREBUILT_KERNEL path.
kernel-prebuilt:
	mkdir -p $(REPO_ROOT)/components/os/device-monterey/prebuilt
	cp $(REPO_ROOT)/components/kernel/build/out/arch/arm64/boot/Image.gz-dtb \
	   $(REPO_ROOT)/components/os/device-monterey/prebuilt/Image.gz-dtb

# Phase 2 milestone (research-notes/18 step 5): lunch + a bootimage/systemimage build, nothing more
# yet. Needs device-tree-link and kernel-prebuilt to have been run first.
lunch: device-tree-link kernel-prebuilt
	cd $(LINEAGEOS_DIR) && $(BUILD_ENV) bash -c \
	  'source build/envsetup.sh && lunch lineage_monterey-userdebug && make bootimage systemimage -j$$(nproc)'
endif
