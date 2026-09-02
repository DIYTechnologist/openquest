# ImageBuffer pool resolution — camera pixels, RE'd (2026-09-01)

Follows [[08-vio-status]] and [[07-hal-A-camera-imu-interface]]. Closes the last open camera
question: **how a delivered `FrameSet` resolves to actual pixel bytes** (previously the atomic
FMQ-read hook gave clean per-frame metadata but the pixels were "pool-referenced, not
pointer-reachable"). Source: static RE of `recon/hal-A-2026-08-31/libimagebuffer.so` (17 KB,
un-stripped exports) + `libvrsensors-hidlwrapper.so`. **Not yet run live** — see "next step".

## Answer in one line

Camera pixels are **not** an index we must resolve to a dmabuf ourselves. Each image is an
`OVR::Sensors::ImageBuffer` reconstructed on the consumer from **two `native_handle`s carried
in the `FrameSet`** — one ashmem metadata handle, one gralloc/ION pixel handle. The
reconstruction maps both; intercept it and pixels + timestamp fall out directly.

## The mechanism (verified from disassembly)

Delivery path in `trackingservice` (consumer):

```
FrameSet (FMQ) ──► libvrsensors-hidlwrapper.so
   make_shared<ImageBuffer>(hidl_handle metaH, hidl_handle grallocH)   @hidlwrapper 0x4b7c8
        → hidl_handle::operator const native_handle*()  (x2, x3 → native_handle*)
        → OVR::Sensors::ImageBuffer::ImageBuffer(native_handle* meta, native_handle* gralloc)
                                                              @libimagebuffer.so 0x3598
```

`libvrsensors-hidlwrapper.so` **imports** the ctor (cross-`.so`, via PLT) and it is called
**per FrameSet delivery** (per frame), inside a `make_shared` emplace — not once at setup.
`libimagebuffer.so` is loaded in `trackingservice` from **`/system/lib64`** (system linker
namespace) → a `/data` `LD_PRELOAD` can interpose the ctor, same enabler as the FrameSet hook.

### `ImageBuffer::ImageBuffer(native_handle* meta, native_handle* gralloc)` — what it does
1. `meta->data[0]` (fd at handle +0xc) → `OVR::OS::mapMemory<ImageBufferSharedData>(fd,1,1,0)`
   → `shared_ptr<ImageBufferSharedData>` stored at **`this+0x40`** (ptr) / `+0x48` (ctrl).
2. Wraps `gralloc` in an `android::GraphicBuffer` (`sp<GraphicBuffer>` at **`this+0x50`**),
   using dims/format/usage read from the shared metadata region.
3. `GraphicBuffer::lock(usage, &vaddr, …)` (or `lockYCbCr` for YUV) with
   `vaddr = this+0x60` → **`this+0x60` = CPU-mapped pixel pointer** after the ctor returns.
   `this+0x58` = bool isYCbCr.

### `ImageBuffer` object layout (consumer)
| off | field |
|---|---|
| 0x00 | u64 creation id (global atomic counter) |
| 0x08 | bool |
| 0x10 | `native_handle*` retained (1-fd copy of meta handle) |
| 0x40 | `shared_ptr<ImageBufferSharedData>` → shared metadata region (ptr @0x40, ctrl @0x48) |
| 0x50 | `android::sp<GraphicBuffer>` (the pixel buffer) |
| 0x58 | bool isYCbCr |
| **0x60** | **void\* locked pixel VA** ← the pixels |
| 0x68.. | lock output (android_ycbcr / stride scratch) |

### `ImageBufferSharedData` — the shared ashmem metadata region (mapped by both HAL & consumer)
Populated by the producer ctor `ImageBuffer(uint id, ImageBufferMetadata&, sp<GraphicBuffer>)`
@0x3a6c (fields copied from `ImageBufferMetadata`; buffer size from `lseek(gralloc_fd,SEEK_END)`):
| off | type | field |
|---|---|---|
| 0x00 | u32 | id |
| 0x08 | u32 | width  (640) |
| 0x0c | u32 | height (480) |
| 0x10 | u16 | pixelFormat (OSSDK::Sensors::v3::PixelFormat, 1..17; mono8 path → Android fmt) |
| 0x14 | u32 | layers / bpp |
| 0x18 | u64 | gralloc usage flags |
| 0x20 | u64 | pixel buffer size (bytes) |
| 0x2c | 2×u32 | lock stride/offset scratch |
| **0x38** | **u64** | **timestamp** (from `ImageBufferMetadata+0x30`) |
| 0x3c | u32 | (trailing) |

Because this region is **shared memory updated in place**, `sd+0x38` is an authoritative
per-frame timestamp readable directly from the consumer.

`PixelFormat` switch (producer & consumer identical), enum→Android format:
2→0x20, 3→0x23, (mono8)→`getMono8AndroidPixelFormat()`, 6→0x3, 7→0x21, else→1. Tracking cams
are mono8 (OV7251), so the `lock()` (non-YCbCr) path is taken and pixels are 8bpp 640×480.

## The tap (built, not yet run): `tools/cam_tap/ib_hook.c`

LD_PRELOAD into `trackingservice`; asm-trampoline interposes
`_ZN3OVR7Sensors11ImageBufferC1EPK13native_handleS4_` (GOT-relative `g_real` via `dlsym`,
same pattern as `fs_atomic.c`). On each call: run the real ctor, then read
`sd=*(this+0x40)`, `px=*(this+0x60)`, and `{w,h,fmt,ts}` from `sd`; dump `w*h` pixel bytes +
a `ts` index line (GO-gated, frame-budgeted). camId = round-robin within each 4-image FrameSet
(cams 0..3 constructed in order), to be confirmed against the FrameSet `camId` from the atomic
hook on the first live run.

### Why this beats the earlier taps
- vs. `img_shim.c` pixel-scanner: no `/proc/self/maps` change-detection or per-frame hashing;
  we get the exact mapped VA the consumer itself uses, at the exact moment it's valid.
- vs. `fs_atomic.c` FMQ-read hook: that fired *before* `make_shared<ImageBuffer>` mapped the
  pixels (hence "not pointer-reachable"). The ctor is the correct, later point.
- Timestamp is read from shared memory (`sd+0x38`), no RANSAC clock-mapping needed for the
  image side (still needed to relate to the syncboss/nRF IMU clock — see [[08-vio-status]]).

## Next step (needs "go" — live preload into trackingservice; frames flow only while worn+moving)
1. Build `ib_hook.so` (plain C, `clang -O2 -fPIC -shared -ldl`).
2. Preload into `trackingservice` (system namespace ok). **Restart-cascade hazard applies** —
   iterate sparingly; `setenforce 0` for the preload, restore Enforcing after.
3. GO-triggered capture: confirm the ctor fires ~120/s (4 cams × 30 Hz), dump a few frames,
   verify pixels are real (min/max spread) and `sd+0x38` timestamps are monotonic, and check
   camId round-robin vs. the atomic FrameSet camId.
4. Fold into the dense stereo capture → longer `exports/vio-precise` trajectory.

## LIVE RUN — VERIFIED (2026-09-01, headset worn+moving)

Preloaded `ib_hook.so` into `trackingservice` via `ts_ib.sh` (`setenforce 0`; stop+pkill;
re-exec with `LD_PRELOAD`, run detached with `setsid` so the pkill can't SIGKILL the adb shell).

**Result: complete success — dense, clean camera frames.**
- Hook loaded (`real=0x751a774598`); **575 ctor calls** in ~5 s, **240 frames** dumped (budget cap).
- **Pixels are real, full-quality** — rendered `frame_00000/01.png`: a sharp fisheye view of the
  room; frame 0 vs frame 1 show clear horizontal parallax = a genuine **stereo pair**.
  (`exports/ib-capture-2026-09-01/`.)
- **Dimensions = 640×481** (not 480 — 481 rows), mono8, `fmt=1`, 307840 B/frame. min=0 max=255
  mean~72 — natural camera dynamic range.
- **camId = `id & 3`** — the ctor `id` (ImageBufferSharedData+0x00) increments per frame and
  cycles the 4 cameras in order 0,1,2,3. Clean round-robin; no correlation with the atomic
  FrameSet hook needed to label cameras.
- ~**120 frames/s** aggregate (4 cams × 30 Hz), fires only during active tracking (as expected).
- Trampoline never crashed TS; device restored cleanly (init `start trackingservice`, Enforcing).

### Caveat found live: `ImageBufferSharedData+0x38` timestamp reads **0** on the consumer
The shared-memory timestamp I expected at `sd+0x38` is **not populated on the receiver side**
(all frames `ts=0`). So either the producer writes the exposure ts elsewhere, or the consumer
maps a zero-initialized view and the real per-frame ts rides the `FrameSet` (the `w11`/exposure
fields the `fs_atomic` hook already reads), not the shared region. **Usable now:** the hook
records a precise per-frame **host `CLOCK_MONOTONIC`** stamp at ctor time (sub-ms after
delivery), monotonic across the capture. **To refine to true exposure ts:** either (a) dump a
wider `sd` window to locate the real ts offset, or (b) run `ib_hook` + `fs_atomic` together and
join on host time → attach each frame the FrameSet `w11` exposure ts (then the existing
nRF/IMU clock map from [[08-vio-status]] links it to the syncboss IMU). Path (b) is the robust
one and reuses machinery already built.

### Reusable assets left in place
- `tools/cam_tap/ib_hook.{c,so}`; `ib_hook.so` also staged on device at
  `/data/local/tmp/ib_hook.so` for the next capture.
- Capture: `exports/ib-capture-2026-09-01/` (240 `.gray` + `ib.log` + `ib.idx`, sample PNGs).

## Exposure-timestamp refinement — RESOLVED (2026-09-01, run 2 with wider metadata dump)

Second ib-only capture logged `sd[0..0x60]` and `this[0..0x68]` for the first 8 frames.
**Conclusive: the exposure timestamp is NOT in the ImageBuffer object graph.**

`ImageBufferSharedData` as the consumer sees it (u64 words by byte offset, live):
```
0x00=id  0x08=<h<<32|w>=0x1e1|0x280 (481×640)  0x10=<layers<<32|fmt>=1|1
0x18=usage=0x20033  0x20=bufsize=0x4c000  0x28=stride=0x280(640)  0x30=0  0x38=0  0x40+=0
```
Everything past 0x28 is **zero** — `sd+0x38` (the field the *producer* ctor writes ts into) is
0 on the receiver. So the HAL does not populate the timestamp in the shared region handed to
trackingservice; **the exposure ts travels in the `FrameSet`** (the `fs_atomic` per-image
ts1/ts2/ts3 fields), not the ImageBuffer.

`this[]` confirmed live exactly as REd: `+0x18` type ptr (constant `0x7d76e10558`),
`+0x40` sd ptr, `+0x48` sd ctrl, `+0x50` sp<GraphicBuffer>, `+0x58` isYCbCr=**0** (mono),
`+0x60` pixel VA (== `px`). Note **sd VA and pixel VA change every frame** (walking mmap, sd
stepped down 0x1000/frame) — so a persistent-VA poller would fail; the per-frame ctor hook is
the correct tap.

### Consequence for precise per-frame exposure timestamps — two viable paths
1. **Merge the FrameSet read hook into `ib_hook`** (single `.so`, single preload). The
   `DualStreamHandle<FrameSet>::read()` and the 4 `ImageBuffer` ctors run sequentially in the
   same thread, so after each `read()` we log the FrameSet descriptor (52 words incl. per-image
   ts) and each following ctor (`camId = id & 3`) is joined to it by host-time order. **One
   preload = stable.** (Loading `ib_hook.so` **and** `fs_atomic.so` as *two* preloads
   destabilised trackingservice — it loaded both, wrote both init lines, then exited before
   producing frames. A single merged library avoids that.) ← recommended next.
   **BUILT: `tools/cam_tap/ibfs_hook.{c,so}`** — both trampolines (ctor + read) in one library,
   separate `g_real_ctor`/`g_real_read`. Not yet run live; validates in one GO capture (confirm
   `read()` fires, TS stays up, and the FrameSet per-image ts vs. ctor `camId=id&3` line up).
2. **Host-clock VIO (no hardware ts).** `ib_hook` already stamps each frame with
   `CLOCK_MONOTONIC` at ctor time. Capture the syncboss IMU host-stamped on the *same* clock
   (as `img_shim`/`sb.rec` already do) → frames and IMU share one clock with **no RANSAC/nRF
   mapping needed at all**. Cost: OS-scheduling jitter vs. the true hardware sample/exposure
   instant (worse than path 1's nRF ts, but simpler). Usable as an immediate fallback.

## Reversibility
Pure read-only interposition of a copy-out path; no device files modified. Same profile as the
existing hooks. Verified: after each run, `trackingservice` restarted clean via init (no preload
in its maps) and SELinux restored to Enforcing. (Restart-cascade note: `stop`+`pkill` can
SIGKILL the adb shell; `start trackingservice` sometimes needs a second try and `setenforce 1`
may need repeating — both recovered cleanly here.)
