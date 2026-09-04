#!/bin/sh
set -e
cd "$(dirname "$0")"
NDK=${NDK:-$(pwd)/../android-ndk-r27c}
CC=$(ls "$NDK"/toolchains/llvm/prebuilt/*/bin/aarch64-linux-android29-clang | head -1)
"$CC" -O2 -o sb_survey sb_survey.c
echo "built $(pwd)/sb_survey"
