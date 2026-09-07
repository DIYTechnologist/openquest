#!/bin/sh
# Compile OpenVINS' ov_core/ov_init/ov_msckf sources for arm64 Android. Runs inside the
# quest-openvins-android-deps container (see ../../Makefile). Object cache keyed by content hash so
# re-running after touching one file doesn't recompile the other ~200 -- OpenVINS has no CMake path
# for a plain cross-compile, so this replaces teaching its CMake to cross-compile (originally
# tools/openvins-android/build.sh).
#
# Deliberately EXCLUDED: ov_init/src/ceres/* and DynamicInitializer.cpp. They pull in Ceres, which
# would be a substantial extra port, and the on-device config runs static initialisation only
# (init_dyn_use: false -- research-notes/53 covers when the host-side validation path uses dynamic
# init instead, which never runs on-device). dyninit_stub.cpp keeps the link honest.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
D=${OPENVINS_DEPS:-/opt/deps}
NDK=${NDK:-/opt/android-ndk-r27c}
CXX=$(ls "$NDK"/toolchains/llvm/prebuilt/*/bin/aarch64-linux-android29-clang++ | head -1)
OUT=$HERE/build
mkdir -p "$OUT"

OCV=$D/OpenCV-android-sdk/sdk/native
INC="-I$D/open_vins/ov_core/src -I$D/open_vins/ov_init/src -I$D/open_vins/ov_msckf/src \
     -I$D/eigen-3.4.0 -I$D/boost_1_84_0 -I$OCV/jni/include -I$HERE/src"
# -DNDEBUG matters: OpenVINS is assert-heavy and this measures/runs live, not debugging.
FLAGS="-O3 -DNDEBUG -std=c++17 -fPIC -Wno-everything $INC"

SRC=""
for d in ov_core/src ov_init/src ov_msckf/src; do
  SRC="$SRC $(find "$D/open_vins/$d" -name '*.cpp' \
      ! -path '*/ceres/*' ! -name 'DynamicInitializer.cpp' \
      ! -name 'test_*' ! -path '*/ros/*' ! -name 'ros*' ! -name '*Visualizer*' ! -name 'run_*')"
done
echo "compiling $(echo $SRC | wc -w) OpenVINS sources"

OBJ=""
for f in $SRC; do
  o="$OUT/$(echo "$f" | md5sum | cut -c1-12).o"
  [ -f "$o" ] && [ "$o" -nt "$f" ] || "$CXX" $FLAGS -c "$f" -o "$o"
  OBJ="$OBJ $o"
done
"$CXX" $FLAGS -c "$HERE/src/dyninit_stub.cpp" -o "$OUT/stub.o"
"$CXX" $FLAGS -c "$HERE/src/boostfs_stub.cpp" -o "$OUT/boostfs.o"
echo "$OBJ $OUT/stub.o $OUT/boostfs.o" > "$OUT/objs.list"
