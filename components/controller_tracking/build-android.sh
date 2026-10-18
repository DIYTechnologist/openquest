#!/bin/sh
# SPDX-License-Identifier: MIT
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
: "${ANDROID_NDK_HOME:?Set ANDROID_NDK_HOME to Android NDK 26.1.10909125}"
if ! grep -q 'Pkg.Revision = 26.1.10909125' "$ANDROID_NDK_HOME/source.properties"; then
    echo 'This build is pinned to Android NDK 26.1.10909125.' >&2;exit 1
fi
case $(uname -s) in Darwin) HOST=darwin-x86_64;; Linux) HOST=linux-x86_64;; *) echo 'Unsupported build host' >&2;exit 1;; esac
TOOLCHAIN="$ANDROID_NDK_HOME/toolchains/llvm/prebuilt/$HOST"
CXX="$TOOLCHAIN/bin/aarch64-linux-android29-clang++"
CACHE=${OPENQUEST_DEPS_DIR:-"$ROOT/.deps"}
SDK=${OPENCV_ANDROID_SDK:-"$CACHE/OpenCV-android-sdk"}
mkdir -p "$CACHE" "$ROOT/prebuilts/arm64" "$ROOT/prebuilts/licenses"
if [ ! -d "$SDK/sdk/native/jni/include" ]; then
    ARCHIVE="$CACHE/opencv-4.12.0-android-sdk.zip"
    if [ ! -f "$ARCHIVE" ]; then
        curl -fL --retry 2 -o "$ARCHIVE.download" https://github.com/opencv/opencv/releases/download/4.12.0/opencv-4.12.0-android-sdk.zip
        mv "$ARCHIVE.download" "$ARCHIVE"
    fi
    python3 - "$ARCHIVE" <<'PY'
import hashlib,sys
from pathlib import Path
expected='fd7f2332331b4eb8b67e55137281cfb16823c9399d90deb9cfa3476783b99e35'
if hashlib.sha256(Path(sys.argv[1]).read_bytes()).hexdigest()!=expected:raise SystemExit('OpenCV SDK checksum mismatch')
PY
    unzip -q -o "$ARCHIVE" 'OpenCV-android-sdk/sdk/native/jni/include/*' 'OpenCV-android-sdk/sdk/native/libs/arm64-v8a/*' 'OpenCV-android-sdk/sdk/etc/licenses/*' -d "$CACHE"
fi
LIB="$SDK/sdk/native/libs/arm64-v8a"
for PROGRAM in service replay selftest live; do
    "$CXX" -std=c++17 -O2 -fPIC -Wall -Wextra -I"$SDK/sdk/native/jni/include" "$ROOT/src/tracker.cpp" "$ROOT/src/$PROGRAM.cpp" -L"$LIB" -lopencv_java4 -o "$ROOT/prebuilts/arm64/oq-controller-$PROGRAM"
done
"$CXX" -std=c++17 -O2 -I"$ROOT/src" "$ROOT/tests/socket_test.cpp" -o "$ROOT/prebuilts/arm64/oq-controller-socket-test"
"$CXX" -std=c++17 -O2 -Wall -Wextra -I"$SDK/sdk/native/jni/include" -I"$ROOT/src" "$ROOT/tests/live_input_test.cpp" -L"$LIB" -lopencv_java4 -o "$ROOT/prebuilts/arm64/oq-controller-input-test"
cp "$LIB/libopencv_java4.so" "$ROOT/prebuilts/arm64/"
cp "$TOOLCHAIN/sysroot/usr/lib/aarch64-linux-android/libc++_shared.so" "$ROOT/prebuilts/arm64/"
cp "$SDK/sdk/etc/licenses/"* "$ROOT/prebuilts/licenses/"
cp "$ANDROID_NDK_HOME/NOTICE" "$ROOT/prebuilts/licenses/NDK-NOTICE"
cp "$ANDROID_NDK_HOME/NOTICE.toolchain" "$ROOT/prebuilts/licenses/NDK-toolchain-NOTICE"
cat "$ROOT/prebuilts/licenses/"* > "$ROOT/prebuilts/NOTICE"
python3 - "$ROOT/prebuilts" <<'PY'
import hashlib,json,sys
from pathlib import Path
p=Path(sys.argv[1]);(p/'SHA256SUMS.json').write_text(json.dumps({f.name:hashlib.sha256(f.read_bytes()).hexdigest() for f in sorted((p/'arm64').iterdir())},indent=2)+'\n')
PY
printf '%s\n' 'Built Android arm64 tracker, service and tests. No device files were modified.'
