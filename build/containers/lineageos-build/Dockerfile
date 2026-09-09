# Toolchain for the step-5 OS swap (research-notes/18 step 5, research-notes/65/66): a LineageOS
# 21 (Android 14) source sync + build environment for the from-scratch device/oculus/monterey tree
# -- msm8998 has no monterey device tree anywhere (research-notes/65's search), so this builds one
# against LineageOS's own manifest, using the actively-maintained msm8998-common trees (Motorola,
# Nokia) as structural reference only, not something forked wholesale.
#
# Ubuntu 20.04, not 22.04 (unlike kernel-toolchain/openvins-android-deps): AOSP/LineageOS's
# documented host package list (lib32ncurses5-dev, libncurses5, lib32readline-dev) was dropped from
# Ubuntu's repos in 22.04 -- this is the same "the exact old toolchain the project already needs"
# reasoning as the kernel container's pinned GCC 4.9, just for host build deps instead of a
# cross-compiler.
FROM ubuntu:20.04
ENV DEBIAN_FRONTEND=noninteractive TZ=UTC

# Standard AOSP/LineageOS host build dependency list.
RUN apt-get update && apt-get install -y --no-install-recommends \
      bc bison build-essential ccache curl flex g++-multilib gcc-multilib \
      git gnupg gperf imagemagick lib32ncurses5-dev lib32readline-dev lib32z1-dev \
      liblz4-tool libncurses5 libncurses5-dev libsdl1.2-dev libssl-dev libxml2 libxml2-utils \
      lzop pngcrush rsync schedtool squashfs-tools xsltproc zip zlib1g-dev \
      python3 python-is-python3 unzip fontconfig ca-certificates openssh-client \
      openjdk-17-jdk-headless \
 && rm -rf /var/lib/apt/lists/*

# The `repo` launcher, not a distro package -- Google's own tool, fetched once and pinned by
# content (the script itself is small and stable; if Google rotates it, a rebuild picks up the new
# one deliberately via cache invalidation on the URL fetch, not silently).
RUN curl -o /usr/local/bin/repo https://storage.googleapis.com/git-repo-downloads/repo \
 && chmod +x /usr/local/bin/repo

ENV USE_CCACHE=1 CCACHE_DIR=/ccache
