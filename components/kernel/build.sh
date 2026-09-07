#!/bin/sh
# Build the monterey (Quest 1, msm8998) kernel, optionally with camera instrumentation.
#
#   ./build.sh            baseline -- config byte-identical to the device except the one
#                         documented deviation in CONFIG_DEVIATIONS.md
#   ./build.sh instrument adds CONFIG_MSMB_CAMERA_DEBUG=y + CONFIG_DYNAMIC_DEBUG=y
#
# Runs inside the quest-kernel-toolchain container (build/containers/kernel-toolchain/Dockerfile),
# which provides the AOSP GCC 4.9 toolchains -- not arbitrary, it's the byte-identical compiler
# string found in the device's own /proc/version (research-notes/21). Invoke via `make` (see this
# component's Makefile), or directly as `podman run ... build.sh [instrument]`.
set -e
cd "$(dirname "$0")"
HERE=$(pwd)
ROOT=$(cd "$HERE/../.." && pwd)
. "$HERE/env.sh"

mkdir -p "$KOUT"
if [ ! -f "$KOUT/.config" ]; then
  zcat "$ROOT/recon/config.gz" > "$KOUT/.config"
  # Meta's GPL release omits verity.x509.pem; the keyring has no consumer here. See
  # CONFIG_DEVIATIONS.md for why this is safe.
  sed -i 's|^CONFIG_SYSTEM_TRUSTED_KEYS=.*|CONFIG_SYSTEM_TRUSTED_KEYS=""|' "$KOUT/.config"
fi

if [ "$1" = "instrument" ]; then
  # Config-only instrumentation -- no driver logic is patched.
  #   MSMB_CAMERA_DEBUG : turns CDBG into pr_debug across the camera stack AND, critically, makes
  #                       msm_csid_set_debug_reg() a real function instead of an empty stub, so the
  #                       CSID interrupt mask is actually programmed. Without it the CSID only ever
  #                       raises reset-done -- which is why "no data interrupts" was never evidence
  #                       of "no data".
  #   DYNAMIC_DEBUG     : the device kernel has this OFF, so every pr_debug compiles to nothing and
  #                       none of the above can be enabled at runtime. This is the whole reason a
  #                       rebuild is required rather than a debugfs toggle.
  sed -i 's|^# CONFIG_MSMB_CAMERA_DEBUG is not set|CONFIG_MSMB_CAMERA_DEBUG=y|' "$KOUT/.config"
  sed -i 's|^# CONFIG_DYNAMIC_DEBUG is not set|CONFIG_DYNAMIC_DEBUG=y|' "$KOUT/.config"
fi

# Quoted NAME=value assignments, not a flat $M string re-split by the shell: HOSTCFLAGS is itself
# several space-separated flags, and an unquoted expansion tears it into separate argv words --
# make's getopt then parses fragments like "-std=gnu89" as bundled short options and fails on '='.
kmake() {
  make -C "$KSRC" O="$KOUT" ARCH=arm64 CROSS_COMPILE="$CROSS_COMPILE" \
    CROSS_COMPILE_ARM32="$CROSS_COMPILE_ARM32" HOSTCFLAGS="$KHOSTCFLAGS" HOSTLDFLAGS="$KHOSTLDFLAGS" "$@"
}
kmake olddefconfig
kmake -j"$(nproc)" Image.gz-dtb
echo "built: $KOUT/arch/arm64/boot/Image.gz-dtb"
