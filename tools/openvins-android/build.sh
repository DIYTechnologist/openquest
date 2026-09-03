#!/bin/sh
# Cross-build OpenVINS + euroc_runner for arm64 Android (step X of notes/18).
#
# Purpose: measure OpenVINS' per-frame cost ON the Quest's own Snapdragon 835. Everything so far has
# run on the host, so nothing is known about whether the estimator fits the 33.3 ms budget at 30 Hz.
# Step 4 (replacing trackingservice in place) is impossible if it does not, so this result can
# invalidate later work and is worth having early.
#
# No CMake: OpenVINS' build assumes ROS/catkin and system packages. Compiling the sources directly
# is less machinery than teaching its CMake to cross-compile, and this only needs to produce one
# static binary.
#
# Dependencies staged under work/android-deps by fetch.sh:
#   OpenCV-android-sdk  prebuilt arm64 static libs (the only non-header dependency)
#   boost_1_84_0        headers only -- posix_time and math are header-only
#   eigen-3.4.0         headers only
#   open_vins           source, copied out of the openvins:runner image
#
# Deliberately EXCLUDED: ov_init/src/ceres/* and DynamicInitializer.cpp. They pull in Ceres, which
# would be a substantial extra port, and we run static initialisation only (init_dyn_use: false),
# so the dynamic initialiser is dead code for our configuration. InertialInitializer references it
# only behind that flag; a stub keeps the link honest and will abort loudly if it is ever reached.
set -e
cd "$(dirname "$0")/../.."
ROOT=$(pwd)
D=$ROOT/work/android-deps
NDK=${NDK:-$ROOT/tools/android-ndk-r27c}
TC=$(ls -d "$NDK"/toolchains/llvm/prebuilt/*)
CXX=$TC/bin/aarch64-linux-android29-clang++
OUT=$ROOT/work/android-deps/build
mkdir -p "$OUT"

OCV=$D/OpenCV-android-sdk/sdk/native
INC="-I$D/open_vins/ov_core/src -I$D/open_vins/ov_init/src -I$D/open_vins/ov_msckf/src \
     -I$D/eigen-3.4.0 -I$D/boost_1_84_0 -I$OCV/jni/include -I$ROOT/tools/openvins-android"

# -DNDEBUG matters: OpenVINS is assert-heavy and we are measuring speed, not debugging.
FLAGS="-O3 -DNDEBUG -std=c++17 -fPIC -Wno-everything $INC"

SRC=""
for d in ov_core/src ov_init/src ov_msckf/src; do
  SRC="$SRC $(find "$D/open_vins/$d" -name '*.cpp' \
      ! -path '*/ceres/*' ! -name 'DynamicInitializer.cpp' \
      ! -name 'test_*' ! -path '*/ros/*' ! -name 'ros*' ! -name '*Visualizer*' ! -name 'run_*')"
done
echo "compiling $(echo $SRC | wc -w) sources"

OBJ=""
for f in $SRC; do
  o="$OUT/$(echo "$f" | md5sum | cut -c1-12).o"
  [ -f "$o" ] && [ "$o" -nt "$f" ] || "$CXX" $FLAGS -c "$f" -o "$o"
  OBJ="$OBJ $o"
done
"$CXX" $FLAGS -c "$ROOT/tools/openvins-android/dyninit_stub.cpp" -o "$OUT/stub.o"
"$CXX" $FLAGS -c "$ROOT/tools/openvins-android/boostfs_stub.cpp" -o "$OUT/boostfs.o"
"$CXX" $FLAGS -c "$ROOT/tools/openvins-android/bench_runner.cpp" -o "$OUT/bench.o"

# OpenCV static libs must precede their 3rdparty deps on the link line.
OCVLIBS="$OCV/staticlibs/arm64-v8a/libopencv_video.a \
         $OCV/staticlibs/arm64-v8a/libopencv_calib3d.a \
         $OCV/staticlibs/arm64-v8a/libopencv_features2d.a \
         $OCV/staticlibs/arm64-v8a/libopencv_flann.a \
         $OCV/staticlibs/arm64-v8a/libopencv_imgproc.a \
         $OCV/staticlibs/arm64-v8a/libopencv_imgcodecs.a \
         $OCV/staticlibs/arm64-v8a/libopencv_core.a \
         $(ls $OCV/3rdparty/libs/arm64-v8a/*.a)"

"$CXX" $FLAGS -o "$OUT/ov_bench" $OBJ "$OUT/stub.o" "$OUT/boostfs.o" "$OUT/bench.o" \
    $OCVLIBS -static-libstdc++ -llog -lz -lm -ldl
echo "built $OUT/ov_bench"
file "$OUT/ov_bench" 2>/dev/null | head -1
