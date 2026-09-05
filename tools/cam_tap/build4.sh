#!/bin/sh
set -e
cd "$(dirname "$0")"
NDK=${NDK:-$(pwd)/../android-ndk-r27c}
CC=$(ls "$NDK"/toolchains/llvm/prebuilt/*/bin/aarch64-linux-android29-clang | head -1)
"$CC" -O2 -fPIC -shared -o ibfs_hook4.so ibfs_hook4.c -ldl
echo "built $(pwd)/ibfs_hook4.so"
