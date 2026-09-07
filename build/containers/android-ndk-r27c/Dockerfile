# Shared Android cross-compile base for components/{camera,controllers,tracking}. Bakes the NDK
# in at image-build time so no component needs it unpacked on the host. Mirrors the exact URL the
# README's manual "regenerating excluded artifacts" step already used.
FROM ubuntu:22.04
ENV DEBIAN_FRONTEND=noninteractive TZ=UTC
RUN apt-get update && apt-get install -y --no-install-recommends \
      curl unzip make git ca-certificates \
 && rm -rf /var/lib/apt/lists/*
RUN curl -fsSL -o /tmp/ndk.zip \
      https://dl.google.com/android/repository/android-ndk-r27c-linux.zip \
 && unzip -q /tmp/ndk.zip -d /opt && rm /tmp/ndk.zip
ENV NDK=/opt/android-ndk-r27c
