#!/bin/sh
# Build vio_live: the same OpenVINS objects tools/openvins-android/build.sh produces, plus our live
# stream consumer and binder injection. Run tools/openvins-android/build.sh first (it caches the
# ~200 OpenVINS objects under work/android-deps/build).
set -e
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
D=$ROOT/work/android-deps
NDK=${NDK:-$ROOT/tools/android-ndk-r27c}
TC=$(ls -d "$NDK"/toolchains/llvm/prebuilt/*)
CXX=$TC/bin/aarch64-linux-android29-clang++
OUT=$D/build
OCV=$D/OpenCV-android-sdk/sdk/native
INC="-I$D/open_vins/ov_core/src -I$D/open_vins/ov_init/src -I$D/open_vins/ov_msckf/src \
     -I$D/eigen-3.4.0 -I$D/boost_1_84_0 -I$OCV/jni/include -I$ROOT/tools/openvins-android"
FLAGS="-O3 -DNDEBUG -std=c++17 -fPIC -Wno-everything $INC"

[ -d "$OUT" ] || { echo "run tools/openvins-android/build.sh first"; exit 1; }
OBJ=$(ls "$OUT"/*.o | grep -v -E 'bench\.o$|vio_live\.o$')
"$CXX" $FLAGS -c "$ROOT/tools/vio_live/vio_live.cpp" -o "$OUT/vio_live.o"
OCVLIBS="$OCV/staticlibs/arm64-v8a/libopencv_video.a \
         $OCV/staticlibs/arm64-v8a/libopencv_calib3d.a \
         $OCV/staticlibs/arm64-v8a/libopencv_features2d.a \
         $OCV/staticlibs/arm64-v8a/libopencv_flann.a \
         $OCV/staticlibs/arm64-v8a/libopencv_imgproc.a \
         $OCV/staticlibs/arm64-v8a/libopencv_imgcodecs.a \
         $OCV/staticlibs/arm64-v8a/libopencv_core.a \
         $(ls $OCV/3rdparty/libs/arm64-v8a/*.a)"
"$CXX" $FLAGS -o "$ROOT/tools/vio_live/vio_live" $OBJ "$OUT/vio_live.o" \
    $OCVLIBS -static-libstdc++ -llog -lz -lm -ldl -lbinder_ndk
echo "built $ROOT/tools/vio_live/vio_live"
