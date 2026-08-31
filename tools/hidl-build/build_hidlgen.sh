#!/usr/bin/env bash
# Build hidl-gen (AOSP system/tools/hidl) as a host tool with Homebrew clang.
set -uo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
H="$HERE/hidl"
LB="$HERE/libbase"
AOSP_INC="$HERE/../aosp-headers/inc"     # liblog headers (android/log.h, log/log.h)
OUT="$HERE/obj"; mkdir -p "$OUT"
CXX="clang++"
STD="-std=c++17 -O1 -g0 "
INC="-I$H -I$H/utils/include/hidl-util -I$H/host_utils/include/hidl-util -I$H/hashing/include/hidl-hash -I$H/utils/include -I$H/host_utils/include -I$H/hashing/include -I$LB/include -I$AOSP_INC"
DEF="-D_FILE_OFFSET_BITS=64 -include cstdint -include cstring -include algorithm -include cstdio -include cstdlib -include cctype"

# libbase host sources (skip *_test.cpp, windows, test_utils)
LB_SRCS=$(ls "$LB"/*.cpp | grep -viE '_test\.cpp|test_utils|test_main|errors_windows|utf8')
# hidl top-level (skip tests), + subdir libs
H_SRCS=$(ls "$H"/*.cpp | grep -viE '_test\.cpp')
SUB_SRCS="$H/utils/FQName.cpp $H/utils/FqInstance.cpp $H/host_utils/Formatter.cpp $H/host_utils/StringHelper.cpp $H/hashing/Hash.cpp"

echo "compiling $(echo $LB_SRCS $H_SRCS $SUB_SRCS | wc -w) TUs..."
objs=()
fail=0
for src in $LB_SRCS $H_SRCS $SUB_SRCS; do
  o="$OUT/$(echo "$src" | md5sum | cut -c1-12).o"
  if ! $CXX $STD $INC $DEF -c "$src" -o "$o" 2>>"$OUT/errors.log"; then
    echo "FAIL: $src"; fail=1
  fi
  objs+=("$o")
done
if [ $fail -ne 0 ]; then echo "--- compile errors (tail) ---"; tail -25 "$OUT/errors.log"; exit 1; fi

echo "linking..."
$CXX $STD "${objs[@]}" -o "$HERE/hidl-gen" -lcrypto -lpthread 2>>"$OUT/errors.log" \
  && echo "BUILT: $HERE/hidl-gen" || { echo "--- link errors ---"; tail -30 "$OUT/errors.log"; exit 1; }
