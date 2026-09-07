# Kernel build environment for monterey (Quest 1, msm8998). Sourced by build.sh (which sets
# $HERE=components/kernel before sourcing this), running inside the quest-kernel-toolchain
# container (build/containers/kernel-toolchain/Dockerfile).
#
# Toolchain is GCC 4.9.x 20150123 (prerelease) -- byte-identical to the compiler string in the
# running kernel's /proc/version (research-notes/21), so the build matches what Meta shipped.
export ARCH=arm64
export SUBARCH=arm64
export CROSS_COMPILE=/opt/kbuild/aarch64-linux-android-4.9/bin/aarch64-linux-android-
export KSRC="$HERE/../../work/oculus-kernel"
export KOUT="$HERE/build/out"
# 32-bit toolchain is required only for the arm64 compat vDSO; the kernel Makefile hard-errors
# without it even though nothing else in this build is 32-bit.
export CROSS_COMPILE_ARM32=/opt/kbuild/arm-linux-androideabi-4.9/bin/arm-linux-androideabi-
# Host tools in this 2016-era tree predate GCC 10's -fno-common default, so scripts/dtc fails to
# link with "multiple definition of yylloc". Restore -fcommon for HOST tools only -- the kernel
# itself is built by GCC 4.9 and is unaffected. The container's Ubuntu libssl-dev still ships
# openssl/engine.h (unlike a stripped Homebrew build), so no extra -I is needed for
# scripts/extract-cert.c (CONFIG_SYSTEM_TRUSTED_KEYRING=y in the device's own config).
export KHOSTCFLAGS="-Wall -Wmissing-prototypes -Wstrict-prototypes -O2 -fomit-frame-pointer -std=gnu89 -fcommon -Wno-error"
export KHOSTLDFLAGS=""
