#!/bin/sh
# Build the two on-device probe binaries used to bring up the controller ground-truth reader
# (research-notes/57). blob_detect.py/bootstrap_model.py/pnp_track.py are host-side Python, no
# build step.
set -e
cd "$(dirname "$0")"
NDK=${NDK:-$(pwd)/../android-ndk-r27c}
CC=$(ls "$NDK"/toolchains/llvm/prebuilt/*/bin/aarch64-linux-android29-clang | head -1)
"$CC" -O2 -o remote_pose_probe remote_pose_probe.c -lbinder_ndk -lm
echo "built $(pwd)/remote_pose_probe"
"$CC" -O2 -o controller_pose_log controller_pose_log.c
echo "built $(pwd)/controller_pose_log"
