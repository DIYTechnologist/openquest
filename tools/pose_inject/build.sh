#!/bin/sh
set -e
cd "$(dirname "$0")"
NDK=${NDK:-$(pwd)/../android-ndk-r27c}
CC=$(ls "$NDK"/toolchains/llvm/prebuilt/*/bin/aarch64-linux-android29-clang | head -1)
"$CC" -O2 -o pose_inject pose_inject.c -lbinder_ndk -lm
echo "built $(pwd)/pose_inject"
