# CHECKPOINT — camera tap (resume file), 2026-09-01

Read this first on resume. Goal of the project: replace Meta's VR blobs on a rooted Quest 1
(`monterey`, msm8998) with an open stack (Monado/Basalt). This checkpoint covers the **camera
frame tap** milestone. Broader context: [[quest1-open-vr-project]] (memory), `notes/08-vio-status.md`,
`notes/09-imagebuffer-pool-re.md` (the full RE), `QWEN.md` (shared handoff log).

## STANDING RULE (do not break)
Before starting ANY capture that needs the headset worn/moving, **prompt and wait for the user to
type "go"** — they may not see the message in time otherwise. Also: **pull raw captures to the host
before deleting them on-device** (headset captures aren't trivially regenerable).

## Device state at checkpoint
- trackingservice: **running (init), Enforcing, clean** (no preload in maps). Pid drifts on restart.
- On-device reusable assets (kept): `/data/local/tmp/{ibfs_hook.so, ib_hook.so, fs_atomic.so,
  ts_ibfs1.sh, mempeek, lldb-server, dmabuf_read, ...}`. No `cap/` dir (cleaned).
- Recovery: boot images backed up at `backups/boot-monterey/` (see [[boot-recovery-backups]]).

## WHAT IS DONE (validated live)
**The camera tap works: dense, full-quality stereo frames from trackingservice.**
- Mechanism (RE'd + confirmed live): camera pixels are `OVR::Sensors::ImageBuffer` objects
  reconstructed **per frame** in trackingservice from two `native_handle`s carried in the
  `FrameSet` (ashmem metadata + gralloc/ION pixels). Interposing the ctor
  `OVR::Sensors::ImageBuffer::ImageBuffer(native_handle*, native_handle*)`
  (mangled `_ZN3OVR7Sensors11ImageBufferC1EPK13native_handleS4_`, in `/system/lib64/libimagebuffer.so`,
  system namespace → `/data` LD_PRELOAD works) gives, after the ctor returns:
  `this+0x40` = ImageBufferSharedData* (id@0,w@8,h@0xc,fmt@0x10), `this+0x60` = **locked CPU pixel VA**.
- Live: 640×481 mono8, real images (rendered PNGs = sharp fisheye room views; cam0/cam1 = stereo
  pair with visible parallax). **camId = id & 3** (clean 4-cam round-robin, exactly 144 each in a run).
- sd/pixel VAs move every frame (walking mmap) → the per-frame ctor hook is the correct tap;
  a persistent-VA poller would fail.
- The **merged single-preload hook** `tools/cam_tap/ibfs_hook.{c,so}` is stable and fires BOTH:
  the ctor (dense pixels + host CLOCK_MONOTONIC ts, GO-gated raw dump) AND
  `MessageQueue<FrameSet,sync>::read()` (the FrameSet exposure metadata). NOTE: interpose the
  **MessageQueue::read** symbol (mangled `_ZN7android8hardware12MessageQueueIN6vendor...4readEPS7_m`),
  NOT `DualStreamHandle::read` (that template wrapper is inlined → no call site to preempt).

## Captured data (host)
- `exports/ib-capture-2026-09-01/` — first ib-only run: 240 frames (tar), logs, sample PNGs,
  the wider sd/this metadata dump (`ib2.log`) that proved sd+0x38 ts = 0 on the consumer.
- `exports/ibfs-capture-2026-09-01/` — merged-hook run: 576 frames (`ib_frames.tar`), `ib.idx`,
  and `ib.log` with 576 `IB` lines + 600 `FS` lines (the FrameSet elements, 16 u64 words each).

## FrameSet element layout (from ib.log `FS` lines, u64 words by index)
- `w0`  = frame seq counter. **TWO interleaved streams** (`0x7f1x` and `0x6c9x`) = the DualStream;
  each delivers at a rock-solid **40.00ms cadence (25 Hz)**.
- `w4`  = 0x280<<32 | 0x1e0  → 640×480. `w5` = format/stride.
- `w9`, `w10` = doubles (~0.007, 9.0) — exposure/gain scalars.
- `w11` = capture time in a **sensor clock** (ns). `w13` = same capture in **CLOCK_MONOTONIC**
  (≈ host read time, latency ~0.6ms). `w12` ≈ mono, ~13ms before w13.
- `w2`, `w14`: high dword = small indices 0..15 (pool/buffer index or sub-camera; NOT camId 0-3).
- **Clock offset host−w11 ≈ 6.717 s but jitters ±3ms (sd 2.9ms, range 32ms)** — NOT a clean constant,
  so you cannot just subtract a fixed offset to convert IB-ctor host time → sensor clock.

## THE ONE OPEN THREAD (next task)
**Attach a precise per-frame exposure timestamp to each dumped IB frame.** Blocker: the IB ctor
stream (early ~1.24s burst, 4 cams id&3) and the FS read stream (full 4.26s, two 25Hz dual-streams,
indices 0-15) do NOT align 1:1 by host time (median pairing gap ~300ms), and the FS `w2`/`w14`
indices don't obviously map to camId 0-3. Options to resolve, in order of preference:
1. **Decode the dual-stream indexing** — figure out how the two 25Hz FS streams + w2/w14 indices map
   to the 4 IB cameras (id&3), so each IB frame gets its FS `w11`/`w13`. Likely needs a bit more RE
   of `libvrsensors-hidlwrapper.so` `DualStreamHandle`/`CompositeStream` (why 2×25Hz not 4×30Hz? are
   the "4 cams" actually 2 stereo pairs each delivered as a dual-stream element with 2 sub-images?).
2. **Pragmatic fallback (probably good enough): host-clock-on-both + Basalt time-offset calib.**
   Use the IB ctor host CLOCK_MONOTONIC ts for frames AND host-stamp the syncboss IMU on the same
   clock (the `img_shim`/`sb_reader` already does this). Basalt calibrates the constant camera
   processing latency via `cam_time_offset_ns`. No FS correlation needed. This reuses everything
   already built and is the fastest path to a longer VIO trajectory.
3. Hardware-clock path (best accuracy): frames = FS `w11`, IMU = syncboss nRF ts (both sensor clock)
   — needs option 1 solved first to get w11 per frame.

## HOW TO RESUME / RE-CAPTURE (needs a "go")
Launcher already on device: `/data/local/tmp/ts_ibfs1.sh` (setenforce 0; stop+pkill+re-exec TS with
`LD_PRELOAD=/data/local/tmp/ibfs_hook.so`; creates `cap/GO`). Run detached so the cascade can't kill
the adb shell:
```
adb shell 'su -c "setsid sh /data/local/tmp/ts_ibfs1.sh </dev/null >/dev/null 2>&1 &"'
sleep 8                      # user moves headset
# pull cap/ib.log + cap/ib.idx + tar of cap/ib_*.gray to exports/, then:
# RESTORE (headset priority): stop; pkill -9 -f trackingservice; start trackingservice; setenforce 1
```
Restart-cascade hazards (seen, recoverable): `stop`+`pkill` can SIGKILL the adb shell (exit 137);
`start trackingservice` sometimes needs a second try; `setenforce 1` may need repeating. Verify end
state: `getprop init.svc.trackingservice`=running, `grep -c ibfs_hook /proc/<pid>/maps`=0, Enforcing.

## Build
```
NDK=~/diytech/quest/tools/android-ndk-r27c/toolchains/llvm/prebuilt/linux-x86_64/bin
$NDK/aarch64-linux-android35-clang -O2 -fPIC -shared -o ibfs_hook.so ibfs_hook.c -ldl
```
Analysis/join scripts are ad-hoc python over `ib.log` (regex the `IB`/`FS` lines) — see this
session's history if reconstructing.
