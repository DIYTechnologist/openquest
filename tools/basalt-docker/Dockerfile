FROM ubuntu:20.04
ENV DEBIAN_FRONTEND=noninteractive TZ=UTC
RUN apt-get update && apt-get install -y \
    git build-essential gcc g++ wget ca-certificates \
    libtbb-dev libeigen3-dev libglew-dev ccache \
    libjpeg-dev libpng-dev libopencv-dev libfmt-dev openssl libssl-dev \
    liblz4-dev libbz2-dev libboost-all-dev libgtest-dev \
    libgl1-mesa-dev libglu1-mesa-dev libepoxy-dev python3 \
 && rm -rf /var/lib/apt/lists/*
# modern cmake
RUN wget -qO- https://github.com/Kitware/CMake/releases/download/v3.28.3/cmake-3.28.3-linux-x86_64.tar.gz \
    | tar xz -C /opt && ln -sf /opt/cmake-3.28.3-linux-x86_64/bin/* /usr/local/bin/
# pre-vcpkg basalt (system deps + submodule Pangolin/Cereal)
RUN git clone https://gitlab.com/VladyslavUsenko/basalt.git /basalt && cd /basalt \
 && git checkout $(git rev-list -1 --before="2021-08-01" master) \
 && git submodule update --init --recursive
WORKDIR /basalt
RUN mkdir build && cd build \
 && cmake .. -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTS=OFF \
 && make -j$(nproc) basalt_vio 2>&1 | tail -25
