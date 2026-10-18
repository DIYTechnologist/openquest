#!/bin/sh
# SPDX-License-Identifier: MIT
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
: "${ANDROID_NDK_HOME:?Set ANDROID_NDK_HOME to Android NDK 26.1.10909125}"
grep -q 'Pkg.Revision = 26.1.10909125' "$ANDROID_NDK_HOME/source.properties" || exit 1
case $(uname -s) in Darwin) HOST=darwin-x86_64;; Linux) HOST=linux-x86_64;; *) exit 1;; esac
python3 "$ROOT/src/fetch_uapi.py" "$ROOT/.kinc"
"$ANDROID_NDK_HOME/toolchains/llvm/prebuilt/$HOST/bin/aarch64-linux-android29-clang" -O2 -std=c11 -I"$ROOT/.kinc" "$ROOT/src/cam_kernel.c" -o "$ROOT/cam_kernel"
printf '%s\n' 'Built cam_kernel from source and pinned kernel UAPI. No device files modified.'
