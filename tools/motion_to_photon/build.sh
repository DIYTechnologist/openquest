#!/bin/sh
# Build the on-device probes for the motion-to-photon feasibility work (research-notes/62).
set -e
cd "$(dirname "$0")"
NDK=${NDK:-$(pwd)/../android-ndk-r27c}
CC=$(ls "$NDK"/toolchains/llvm/prebuilt/*/bin/aarch64-linux-android29-clang | head -1)
"$CC" -O2 -o fb_probe fb_probe.c
echo "built $(pwd)/fb_probe"
