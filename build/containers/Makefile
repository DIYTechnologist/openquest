# Builds every shared toolchain image once. `make -C components/X` will pull these in
# automatically the first time (podman builds on demand if a referenced tag is missing locally),
# but running this ahead of time makes the one-off network fetches explicit.
CONTAINER ?= podman

.PHONY: all android-ndk-r27c kernel-toolchain openvins-android-deps lineageos-build lineageos18-build
all: android-ndk-r27c kernel-toolchain openvins-android-deps lineageos-build lineageos18-build

android-ndk-r27c:
	$(CONTAINER) build -t quest-ndk-r27c android-ndk-r27c

kernel-toolchain:
	$(CONTAINER) build -t quest-kernel-toolchain kernel-toolchain

# depends on quest-ndk-r27c existing locally (its Dockerfile is FROM quest-ndk-r27c)
openvins-android-deps: android-ndk-r27c
	$(CONTAINER) build -t quest-openvins-android-deps openvins-android-deps

lineageos-build:
	$(CONTAINER) build -t quest-lineageos-build lineageos-build

lineageos18-build:
	$(CONTAINER) build -t quest-lineageos18-build lineageos18-build
