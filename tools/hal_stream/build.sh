#!/usr/bin/env bash
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT=/home/ryanm/diytech/quest
NDK=$ROOT/tools/android-ndk-r27c
CXX="$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android29-clang++"
LIBS=$ROOT/recon/hal-A-2026-08-31/devlibs
INC=$ROOT/tools/aosp-headers/inc

# patched __config_site: force libc++ ABI namespace __ndk1 -> __1 to match device libs
PATCH=/tmp/cxxpatch/c++/v1
mkdir -p "$PATCH"
SITE=$NDK/toolchains/llvm/prebuilt/linux-x86_64/sysroot/usr/include/c++/v1/__config_site
sed 's/__ndk1/__1/' "$SITE" > "$PATCH/__config_site"

"$CXX" -std=c++17 -O1 -fPIE -pie -isystem "$PATCH" -I"$INC" \
  -o "$HERE/hal_stream" "$HERE/hal_stream.cpp" \
  -L"$LIBS" -l:vendor.oculus.hardware.sensors@1.0.so \
  -l:libfmq.so -l:libhidlbase.so -l:libutils.so -l:libc++.so -l:libcutils.so -l:liblog.so \
  -Wl,--allow-shlib-undefined -Wl,-rpath,/system/lib64

echo "built: $HERE/hal_stream"; file "$HERE/hal_stream"
