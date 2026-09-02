# ib-capture-2026-09-01 — dense camera frames via ImageBuffer ctor hook

First live capture proving the ImageBuffer pool RE (notes/09-imagebuffer-pool-re.md).
Tap: `tools/cam_tap/ib_hook.so` LD_PRELOAD'd into trackingservice, interposing
`OVR::Sensors::ImageBuffer::ImageBuffer(native_handle*, native_handle*)`.

- `ib_frames.tar` — 240 raw frames `ib_NNNNN.gray`, **640×481 mono8** (307840 B each).
- `ib.idx` — `dumpseq host_ns sd_ts(=0) id w h camId` per dumped frame.
- `ib.log` — full per-ctor metadata log (575 calls incl. non-dumped).
- `frame_0000{0..3}.png`, `frame_00120.png` — sample renders (frames 0/1 = a stereo pair).

camId = `id & 3` (4 cams round-robin, 0,1,2,3 @30 Hz each). Timestamp: use `host_ns`
(CLOCK_MONOTONIC at ctor) for now; `sd_ts` is 0 on the consumer — see note for the
exposure-ts refinement plan (join with fs_atomic w11).
