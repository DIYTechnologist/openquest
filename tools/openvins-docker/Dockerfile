FROM ubuntu:22.04
ENV DEBIAN_FRONTEND=noninteractive TZ=UTC
RUN apt-get update && apt-get install -y --no-install-recommends \
      build-essential cmake git ca-certificates \
      libeigen3-dev libopencv-dev libboost-all-dev libceres-dev \
 && rm -rf /var/lib/apt/lists/*
# Pinned, not --depth 1 off whatever HEAD is on build day: components/tracking's on-device build
# (build/containers/openvins-android-deps/Dockerfile) checks out this exact commit too, so the
# on-device numbers and this host-side validation environment describe the same code. Bump both
# together if OpenVINS is ever updated.
RUN git clone https://github.com/rpng/open_vins.git /open_vins \
 && git -C /open_vins checkout -q 69488123ed9362dd44b6f28e7f4680abbff1442b
# ENABLE_ROS=OFF makes each package build as a plain CMake library. ov_core -> ov_init -> ov_msckf.
RUN for p in ov_core ov_init ov_msckf; do \
      cmake -S /open_vins/$p -B /build/$p -DCMAKE_BUILD_TYPE=Release -DENABLE_ROS=OFF \
            -DCMAKE_INSTALL_PREFIX=/usr/local > /tmp/$p.log 2>&1 || { tail -25 /tmp/$p.log; exit 1; }; \
      cmake --build /build/$p -j$(nproc) >> /tmp/$p.log 2>&1 || { tail -40 /tmp/$p.log; exit 1; }; \
      cmake --install /build/$p >> /tmp/$p.log 2>&1 || true; \
      echo "built $p"; \
    done
