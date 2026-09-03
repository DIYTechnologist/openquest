#!/bin/sh
# Build cam_kernel (B2) against the published kernel uapi headers.
set -e
cd "$(dirname "$0")"
HERE=$(pwd)
NDK=${NDK:-$HERE/../android-ndk-r27c}
K=$(cd "${K:-$HERE/../../work/oculus-kernel}" && pwd)
CC=$(ls "$NDK"/toolchains/llvm/prebuilt/*/bin/aarch64-linux-android29-clang | head -1)

# Same selective staging as build.sh: only the headers the NDK lacks, or bionic's own
# linux/types.h resolves into the kernel tree and redefines struct sigaction.
rm -rf .kinc2 && mkdir -p .kinc2/media .kinc2/linux
cp "$K"/include/uapi/media/*.h .kinc2/media/
cp "$K"/include/uapi/linux/media.h .kinc2/linux/
cp "$K"/drivers/staging/android/uapi/ion.h .kinc2/linux/
# __user is a sparse annotation, not valid C outside the kernel build.
sed -i 's/__user //g; s/ __user//g' .kinc2/media/*.h .kinc2/linux/*.h
sed -i 's/#define V4L2_PIX_FMT_SGRBG14/#undef V4L2_PIX_FMT_SGRBG14\n#define V4L2_PIX_FMT_SGRBG14/' .kinc2/media/msmb_isp.h

"$CC" -O2 -std=c11 -o cam_kernel cam_kernel.c -I.kinc2
echo "built $HERE/cam_kernel"
