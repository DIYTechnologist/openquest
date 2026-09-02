FROM ubuntu:22.04
ENV DEBIAN_FRONTEND=noninteractive TZ=UTC
RUN apt-get update && apt-get install -y --no-install-recommends \
      build-essential cmake git ca-certificates \
      libeigen3-dev libopencv-dev libboost-all-dev libceres-dev \
 && rm -rf /var/lib/apt/lists/*
RUN git clone --depth 1 https://github.com/rpng/open_vins.git /open_vins
# ENABLE_ROS=OFF makes each package build as a plain CMake library. ov_core -> ov_init -> ov_msckf.
RUN for p in ov_core ov_init ov_msckf; do \
      cmake -S /open_vins/$p -B /build/$p -DCMAKE_BUILD_TYPE=Release -DENABLE_ROS=OFF \
            -DCMAKE_INSTALL_PREFIX=/usr/local > /tmp/$p.log 2>&1 || { tail -25 /tmp/$p.log; exit 1; }; \
      cmake --build /build/$p -j$(nproc) >> /tmp/$p.log 2>&1 || { tail -40 /tmp/$p.log; exit 1; }; \
      cmake --install /build/$p >> /tmp/$p.log 2>&1 || true; \
      echo "built $p"; \
    done
