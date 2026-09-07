# Toolchain for components/kernel: the AOSP GCC 4.9.x prebuilt that is byte-identical to the
# compiler string in the device's own /proc/version (research-notes/21), plus the host build deps
# the 2016-era kernel tree's host tools (dtc, extract-cert) need on a modern distro.
FROM ubuntu:22.04
ENV DEBIAN_FRONTEND=noninteractive TZ=UTC
RUN apt-get update && apt-get install -y --no-install-recommends \
      git ca-certificates make bc bison flex libssl-dev build-essential \
      python3 python-is-python3 kmod cpio rsync \
 && rm -rf /var/lib/apt/lists/*
WORKDIR /opt/kbuild
# Same two prebuilts env.sh already expects, at the same relative names -- CROSS_COMPILE and
# CROSS_COMPILE_ARM32 in components/kernel/env.sh point straight at these.
#
# Pinned to a specific commit, not a branch tip: these repos' default branch has since been
# emptied down to an OWNERS file upstream (a plain `git clone --depth 1` gets nothing usable), so
# the actual toolchain content only exists at historical commits. These are the exact commits from
# the working checkout that produced research-notes/21's byte-identical compiler string (pi-release
# snap, 2018-03-21).
RUN mkdir aarch64-linux-android-4.9 && cd aarch64-linux-android-4.9 && git init -q \
 && git fetch --depth 1 \
      https://android.googlesource.com/platform/prebuilts/gcc/linux-x86/aarch64/aarch64-linux-android-4.9 \
      961622e926a1b21382dba4dd9fe0e5fb3ee5ab7c \
 && git checkout -q FETCH_HEAD
RUN mkdir arm-linux-androideabi-4.9 && cd arm-linux-androideabi-4.9 && git init -q \
 && git fetch --depth 1 \
      https://android.googlesource.com/platform/prebuilts/gcc/linux-x86/arm/arm-linux-androideabi-4.9 \
      cb7b3ac1b7fdb49474ff68761909934d1142f594 \
 && git checkout -q FETCH_HEAD
