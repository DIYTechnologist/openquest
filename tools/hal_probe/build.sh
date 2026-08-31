#!/usr/bin/env bash
# Cross-compile hal_probe for Quest 1 (aarch64 Android 10) against pulled device libs.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
NDK=/home/ryanm/diytech/quest/tools/android-ndk-r27c
CXX="$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android29-clang++"
LIBS=/home/ryanm/diytech/quest/recon/hal-A-2026-08-31/devlibs

# Link against device stubs (undefined-symbol resolution only); the real libs load at runtime.
"$CXX" -O1 -fPIE -pie -Wall -o "$HERE/hal_probe" "$HERE/hal_probe.cpp" \
  -Wl,--allow-shlib-undefined -Wl,--no-as-needed \
  -L"$LIBS" \
  -l:vendor.oculus.hardware.sensors@1.0.so \
  -l:libhidlbase.so -l:libutils.so -l:libc++.so -l:libcutils.so -l:liblog.so -l:libbase.so \
  -Wl,-rpath,/system/lib64

echo "built: $HERE/hal_probe"
file "$HERE/hal_probe"
