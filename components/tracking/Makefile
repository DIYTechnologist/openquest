# components/tracking — replaces oculus.internal.tracking.ITrackingService (trackingservice).
# See docs/tracking.md.
#
# vio_live is the core: consumes components/camera's stdout record stream, runs OpenVINS, injects
# poses via ITrackingDataInjectionService. pose_inject is a standalone replay/test harness for that
# same injection interface (research-notes/23) -- it duplicates vio_live's inline injection code by
# design, as the isolated way to test the interface without the estimator in the loop. replay_feed
# replays a recorded components/camera capture as a live stream so vio_live can be validated on a
# desk (research-notes/26). ov_bench measures OpenVINS' per-frame cost on-device (research-notes/20).
#
# vio_live/ov_bench need the OpenVINS+OpenCV+Boost+Eigen bundle baked into quest-openvins-android-
# deps; pose_inject/replay_feed only need the plain NDK image.
CONTAINER_IMAGE := quest-openvins-android-deps
include ../../build/mk/container.mk

ifeq ($(IN_CONTAINER),1)
NDK  := /opt/android-ndk-r27c
D    := $(OPENVINS_DEPS)
CXX  := $(shell ls $(NDK)/toolchains/llvm/prebuilt/*/bin/aarch64-linux-android29-clang++ | head -1)
CC   := $(shell ls $(NDK)/toolchains/llvm/prebuilt/*/bin/aarch64-linux-android29-clang | head -1)
OUT  := build
OCV  := $(D)/OpenCV-android-sdk/sdk/native
INC  := -Isrc -I$(D)/open_vins/ov_core/src -I$(D)/open_vins/ov_init/src -I$(D)/open_vins/ov_msckf/src \
        -I$(D)/eigen-3.4.0 -I$(D)/boost_1_84_0 -I$(OCV)/jni/include
FLAGS := -O3 -DNDEBUG -std=c++17 -fPIC -Wno-everything $(INC)
OCVLIBS := $(OCV)/staticlibs/arm64-v8a/libopencv_video.a \
           $(OCV)/staticlibs/arm64-v8a/libopencv_calib3d.a \
           $(OCV)/staticlibs/arm64-v8a/libopencv_features2d.a \
           $(OCV)/staticlibs/arm64-v8a/libopencv_flann.a \
           $(OCV)/staticlibs/arm64-v8a/libopencv_imgproc.a \
           $(OCV)/staticlibs/arm64-v8a/libopencv_imgcodecs.a \
           $(OCV)/staticlibs/arm64-v8a/libopencv_core.a \
           $(wildcard $(OCV)/3rdparty/libs/arm64-v8a/*.a)

.PHONY: all clean ov-objs
all: vio_live pose_inject replay_feed ov_bench

# Cached by content hash inside the script; re-running only recompiles what changed.
ov-objs:
	sh build_ov_objs.sh

$(OUT)/vio_live.o: src/vio_live.cpp ov-objs
	$(CXX) $(FLAGS) -c $< -o $@
vio_live: $(OUT)/vio_live.o ov-objs
	$(CXX) $(FLAGS) -o $@ $(OUT)/vio_live.o $$(cat $(OUT)/objs.list) \
	    $(OCVLIBS) -static-libstdc++ -llog -lz -lm -ldl -lbinder_ndk

$(OUT)/bench_runner.o: src/bench_runner.cpp ov-objs
	$(CXX) $(FLAGS) -c $< -o $@
ov_bench: $(OUT)/bench_runner.o ov-objs
	$(CXX) $(FLAGS) -o $@ $(OUT)/bench_runner.o $$(cat $(OUT)/objs.list) \
	    $(OCVLIBS) -static-libstdc++ -llog -lz -lm -ldl

pose_inject: src/pose_inject.c
	$(CC) -O2 -o $@ $< -lbinder_ndk -lm

replay_feed: src/replay_feed.c
	$(CC) -O2 -o $@ $<

clean:
	rm -rf vio_live pose_inject replay_feed ov_bench $(OUT)
endif
