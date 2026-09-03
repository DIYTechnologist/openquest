#!/bin/sh
# Fetch the dependencies for the arm64 OpenVINS benchmark (step X, notes/18). ~1 GB, gitignored.
set -e
cd "$(dirname "$0")/../.."
mkdir -p work/android-deps && cd work/android-deps

# OpenCV is the only non-header dependency; the prebuilt Android SDK ships arm64 static libs.
[ -d OpenCV-android-sdk ] || {
  curl -L -o opencv-android.zip \
    https://github.com/opencv/opencv/releases/download/4.10.0/opencv-4.10.0-android-sdk.zip
  unzip -q -o opencv-android.zip; }

# Boost: headers only. posix_time and math are header-only; the three filesystem symbols OpenVINS
# links are stubbed in tools/openvins-android/boostfs_stub.cpp rather than porting the library.
[ -d boost_1_84_0 ] || {
  curl -L -o boost.tar.gz https://archives.boost.io/release/1.84.0/source/boost_1_84_0.tar.gz
  tar xzf boost.tar.gz boost_1_84_0/boost; }

[ -d eigen-3.4.0 ] || {
  curl -L -o eigen.tar.gz https://gitlab.com/libeigen/eigen/-/archive/3.4.0/eigen-3.4.0.tar.gz
  tar xzf eigen.tar.gz; }

# OpenVINS source, taken from the image we already build and run on the host so the on-device
# numbers describe the same revision.
[ -d open_vins ] || podman run --rm -v "$PWD":/out:z openvins:runner sh -c 'cp -r /open_vins /out/open_vins'
echo "deps ready in $(pwd)"
