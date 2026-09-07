# components/tracking's dependency bundle: the NDK base plus everything
# tools/openvins-android/fetch.sh used to download onto the host into work/android-deps/. OpenCV is
# the only non-header dependency (prebuilt arm64 static libs); Boost and Eigen are headers only.
# OpenVINS source comes straight from the same revision the host-side openvins:runner image builds
# and tests, so the on-device numbers describe the same code.
FROM quest-ndk-r27c
RUN apt-get update && apt-get install -y --no-install-recommends git \
 && rm -rf /var/lib/apt/lists/*
WORKDIR /opt/deps

RUN curl -fsSL -o opencv-android.zip \
      https://github.com/opencv/opencv/releases/download/4.10.0/opencv-4.10.0-android-sdk.zip \
 && unzip -q opencv-android.zip && rm opencv-android.zip

RUN curl -fsSL -o boost.tar.gz \
      https://archives.boost.io/release/1.84.0/source/boost_1_84_0.tar.gz \
 && tar xzf boost.tar.gz boost_1_84_0/boost && rm boost.tar.gz

RUN curl -fsSL -o eigen.tar.gz \
      https://gitlab.com/libeigen/eigen/-/archive/3.4.0/eigen-3.4.0.tar.gz \
 && tar xzf eigen.tar.gz && rm eigen.tar.gz

# Pinned to the same commit as the host-side openvins:runner image (tools/openvins-docker/) that
# tools/vio/*.py validates against, so the on-device build and the host-side validation describe
# the same code -- the original reason tools/openvins-android/fetch.sh copied source out of that
# image instead of cloning independently. A second unpinned clone here would silently drift.
RUN git clone https://github.com/rpng/open_vins.git open_vins \
 && git -C open_vins checkout -q 69488123ed9362dd44b6f28e7f4680abbff1442b

ENV OPENVINS_DEPS=/opt/deps
