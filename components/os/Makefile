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
.PHONY: init sync lfs-pull device-tree-link kernel-prebuilt lunch

init:
	mkdir -p $(LINEAGEOS_DIR) $(LINEAGEOS_DIR)/.home
	cd $(LINEAGEOS_DIR) && $(BUILD_ENV) repo init -u https://github.com/LineageOS/android.git -b lineage-21.0

# -c: current branch only (LineageOS wiki's own recommendation for device bring-up -- full history
# of every branch is not needed and roughly triples the download). --no-clone-bundle: the bundle
# CDN has been unreliable for large syncs before elsewhere in this project (UFS/wifi flakiness,
# research-notes/32) -- prefer the slower but more resumable plain fetch path.
sync:
	cd $(LINEAGEOS_DIR) && $(BUILD_ENV) repo sync -c -j4 --no-clone-bundle --force-sync
	$(MAKE) lfs-pull

# git-lfs install --system (in the container image) registers the smudge FILTER, but that alone
# does not guarantee repo sync's checkout actually fetches real LFS blob content -- found the hard
# way 91% into a build (research-notes/68): the four LFS-tracked projects checked out as valid
# 134-byte pointer files ("version https://git-lfs.github.com/spec/v1 ...", no error anywhere) and
# only failed much later when something tried to treat the pointer text as real zip/APK content.
# Explicit `git lfs pull` per project is the reliable way to force the actual objects down.
LFS_DIRS := external/chromium-webview/prebuilt/arm external/chromium-webview/prebuilt/arm64 \
            external/chromium-webview/prebuilt/x86 external/chromium-webview/prebuilt/x86_64
lfs-pull:
	cd $(LINEAGEOS_DIR) && for d in $(LFS_DIRS); do (cd "$$d" && git lfs pull); done

# NOT a single directory symlink -- Soong's Finder (build/soong/finder/finder.go) only follows
# symlinked FILES, not symlinked DIRECTORIES, unless FollowSymlinks is set (default off, a
# deliberate anti-infinite-loop safeguard). A whole-dir symlink here made device/oculus/monterey
# invisible to the product-makefile discovery scan with no error, just "Cannot locate config
# makefile for product" -- found by checking out/.module_paths/AndroidProducts.mk.list directly
# rather than guessing at the cause. `cp -rs` makes device/oculus/monterey a REAL directory tree
# with each individual FILE symlinked back to components/os/device-monterey -- Finder-visible,
# while still keeping this repo as the single source of truth for their content.
device-tree-link:
	mkdir -p $(LINEAGEOS_DIR)/device/oculus
	rm -rf $(LINEAGEOS_DIR)/device/oculus/monterey
	cp -rs $(REPO_ROOT)/components/os/device-monterey $(LINEAGEOS_DIR)/device/oculus/monterey

# One source of truth for the kernel build (components/kernel, its own container/toolchain) --
# this copies the already-built output in, matching device.mk's TARGET_PREBUILT_KERNEL path, then
# hexpatches it for this device's legacy system-as-root requirement (research-notes/21, /71): the
# stock kernel always skips the boot.img ramdisk unless skip_initramfs is hexpatched to
# want_initramfs. components/kernel's own build has never carried this (it boots today only via
# Magisk separately applying the same patch through magiskboot's LEGACYSAR path) -- for a from-
# scratch AOSP-generated ramdisk, nothing else applies it, so it has to happen here.
kernel-prebuilt:
	mkdir -p $(REPO_ROOT)/components/os/device-monterey/prebuilt
	python3 $(REPO_ROOT)/components/os/patch_legacysar_kernel.py \
	   $(REPO_ROOT)/components/kernel/build/out/arch/arm64/boot/Image.gz-dtb \
	   $(REPO_ROOT)/components/os/device-monterey/prebuilt/Image.gz-dtb

# Phase 2 milestone (research-notes/18 step 5): lunch + a bootimage/systemimage build, nothing more
# yet. kernel-prebuilt MUST run before device-tree-link: it places Image.gz-dtb inside
# components/os/device-monterey/prebuilt/ in the SOURCE tree, and device-tree-link's `cp -rs` only
# picks up files that already exist there at the time it runs.
#
# BUILD_JOBS defaults to 8, not $(nproc) (16 on this host): a first attempt at -j16 hit a real,
# confirmed host-wide OOM kill (journalctl -k: "oom-kill ... task=ninja", triggered by desktop
# memory pressure competing with the build, not a hard "this build needs more than 30GB" ceiling --
# several of ninja's concurrent build actions here are JVM processes, kotlinc/R8/javac, that can
# each spike a few GB, so -j16 means up to 16 of those concurrently). Override on the command line
# (`make -C components/os lunch BUILD_JOBS=N`) if running on a host with more headroom.
BUILD_JOBS ?= 8
lunch: kernel-prebuilt device-tree-link
	cd $(LINEAGEOS_DIR) && $(BUILD_ENV) bash -c \
	  'source build/envsetup.sh && lunch lineage_monterey-ap2a-userdebug && make bootimage systemimage -j$(BUILD_JOBS)'
endif

# --- lineage-18.1 (Android 11) bring-up reference build, research-notes/74 -------------------
#
# A SEPARATE tree and container from everything above: validates this kernel's real requirement
# (the old monolithic/system-as-root ramdisk, BOARD_BUILD_SYSTEM_ROOT_IMAGE -- obsolete on
# lineage-21) on an AOSP version old enough to still natively support it, before hand-building an
# equivalent ramdisk for the lineage-21 tree. container.mk's wrapper only supports one
# CONTAINER_IMAGE for the whole file, so these targets invoke podman directly with the -18
# container instead of going through it -- an explicit rule with its own recipe takes precedence
# over container.mk's generic `%:` pattern rule regardless of include order.
LINEAGEOS18_DIR := $(REPO_ROOT)/work/lineageos18
CONTAINER18 := podman run --rm -v $(REPO_ROOT):$(REPO_ROOT):Z -w $(LINEAGEOS18_DIR) quest-lineageos18-build
BUILD_ENV18 := HOME=$(LINEAGEOS18_DIR)/.home \
               GIT_AUTHOR_NAME="Quest OS Build" GIT_AUTHOR_EMAIL="quest-build@localhost" \
               GIT_COMMITTER_NAME="Quest OS Build" GIT_COMMITTER_EMAIL="quest-build@localhost"

.PHONY: init-18 sync-18 lfs-pull-18 device-tree-link-18 kernel-prebuilt-18 lunch-18

init-18:
	mkdir -p $(LINEAGEOS18_DIR) $(LINEAGEOS18_DIR)/.home
	$(CONTAINER18) bash -c '$(BUILD_ENV18) repo init -u https://github.com/LineageOS/android.git -b lineage-18.1'

sync-18:
	$(CONTAINER18) bash -c '$(BUILD_ENV18) repo sync -c -j4 --no-clone-bundle --force-sync'
	$(MAKE) lfs-pull-18

lfs-pull-18:
	$(CONTAINER18) bash -c 'for d in $(LFS_DIRS); do (cd "$$d" && git lfs pull) || true; done'

device-tree-link-18:
	mkdir -p $(LINEAGEOS18_DIR)/device/oculus
	rm -rf $(LINEAGEOS18_DIR)/device/oculus/monterey
	cp -rs $(REPO_ROOT)/components/os/device-monterey-18 $(LINEAGEOS18_DIR)/device/oculus/monterey

# Same patched kernel as the lineage-21 tree -- one source of truth, copied into this tree's own
# device-monterey-18/prebuilt/ (kept separate from device-monterey/prebuilt/ so the two trees'
# device-tree-link targets never race or overwrite each other).
kernel-prebuilt-18:
	mkdir -p $(REPO_ROOT)/components/os/device-monterey-18/prebuilt
	python3 $(REPO_ROOT)/components/os/patch_legacysar_kernel.py \
	   $(REPO_ROOT)/components/kernel/build/out/arch/arm64/boot/Image.gz-dtb \
	   $(REPO_ROOT)/components/os/device-monterey-18/prebuilt/Image.gz-dtb

lunch-18: kernel-prebuilt-18 device-tree-link-18
	$(CONTAINER18) bash -c \
	  '$(BUILD_ENV18) bash -c "source build/envsetup.sh && lunch lineage_monterey-userdebug && make bootimage systemimage -j$(BUILD_JOBS)"'
