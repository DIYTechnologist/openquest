# Toolchain for the LineageOS 18.1 (Android 11) bring-up reference build (research-notes/74):
# validate this device's kernel actually boots with the OLD monolithic (BOARD_BUILD_SYSTEM_ROOT_IMAGE)
# ramdisk style it needs, on an AOSP version old enough to still natively produce that shape --
# BOARD_BUILD_SYSTEM_ROOT_IMAGE is KATI_obsolete_var on lineage-21 (research-notes/72), so this
# needs its own, older build environment, not the lineage-21 quest-lineageos-build image.
#
# Ubuntu 20.04, same reasoning as the other build containers (lib32ncurses5-dev etc. dropped from
# 22.04's repos). JDK: Android 11-era AOSP is documented as needing OpenJDK 9, which Ubuntu 20.04's
# own repos no longer carry (long EOL) -- starting with openjdk-11-jdk (Ubuntu 20.04's own default)
# since LineageOS has historically backported newer-JDK acceptance to older branches; adjust if the
# actual build complains, rather than pre-emptively chasing down an EOL JDK 9 package.
FROM ubuntu:20.04
ENV DEBIAN_FRONTEND=noninteractive TZ=UTC

RUN apt-get update && apt-get install -y --no-install-recommends \
      bc bison build-essential ccache curl flex g++-multilib gcc-multilib \
      git git-lfs gnupg gperf imagemagick lib32ncurses5-dev lib32readline-dev lib32z1-dev \
      liblz4-tool libncurses5 libncurses5-dev libsdl1.2-dev libssl-dev libxml2 libxml2-utils \
      lzop pngcrush rsync schedtool squashfs-tools xsltproc zip zlib1g-dev \
      python3 python2 python-is-python3 unzip fontconfig ca-certificates openssh-client \
      openjdk-11-jdk-headless \
 && rm -rf /var/lib/apt/lists/* \
 && git lfs install --system

RUN curl -o /usr/local/bin/repo https://storage.googleapis.com/git-repo-downloads/repo \
 && chmod +x /usr/local/bin/repo

ENV USE_CCACHE=1 CCACHE_DIR=/ccache
