#!/bin/sh
# Build the ioctl tracer against the PUBLISHED kernel uapi headers, so every ioctl code is computed
# by the compiler from the same definitions the kernel uses rather than transcribed.
set -e
cd "$(dirname "$0")"
HERE=$(pwd)
NDK=${NDK:-$HERE/../android-ndk-r27c}
K=$(cd "${K:-$HERE/../../work/oculus-kernel}" && pwd)
CC=$(ls "$NDK"/toolchains/llvm/prebuilt/*/bin/aarch64-linux-android29-clang | head -1)

sh ./gen_ioctl_table.sh "$K"

# Stage ONLY the media/ headers. Putting $K/include/uapi on the include path makes bionic's own
# #include <linux/types.h> resolve to kernel headers, which then redefine struct sigaction and
# friends. We want the MSM media definitions and the NDK's linux/ headers, not both trees.
rm -rf .kinc && mkdir -p .kinc/media .kinc/linux
cp "$K"/include/uapi/media/*.h .kinc/media/
# Only the two linux/ headers the NDK lacks. Staging the whole linux/ tree is what caused the
# sigaction collision; staging exactly these leaves linux/types.h and friends coming from the NDK.
cp "$K"/include/uapi/linux/media.h .kinc/linux/
cp "$K"/drivers/staging/android/uapi/ion.h .kinc/linux/       # ION is Android-only, not in uapi/linux

"$CC" -shared -fPIC -O2 -o libioctl_trace.so ioctl_trace.c -I. -I.kinc -ldl
echo "built $HERE/libioctl_trace.so"
