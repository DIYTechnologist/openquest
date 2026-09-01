# QWEN.md — live handoff note (read first)

Date: 2026-08-31. Device: `1PASH9ACHD0215` / `monterey` (Quest 1, msm8998), Magisk root.
Goal: open VR stack (Monado/Basalt) fed by 4× OV7251 tracking cams + ICM-20602 IMU,
replacing Meta's closed blobs.

## The two options (current framing)

- **Option 1 — syncboss-direct**: read raw IMU from the open `oculus_syncboss` kernel
  driver (`/dev/syncboss_stream0`, miscfifo, fans out to every reader). Proven: live
  ~30 Hz broadcast packets readable as root (20-byte `01 03 00 e0 00 0e 00` header +
  ts32 + ctr16 + seq8). Missing: the 1 kHz IMU is *not* in the broadcast — it must be
  enabled via `/dev/syncboss_control0` (wire protocol RE needed). Gives raw IMU only —
  **no cameras** (those are MIPI/CSI through the camera pipeline).
- **Option 2 — beat the closed HAL** (`vendor.oculus.hardware.sensors@1.0-service`):
  our open `ISensorClient` + FMQ client is proven byte-identical to Meta's own
  `trackingservice` client (LD_PRELOAD intercept captured the real recipe:
  `prepareStream` → `streamControl(cmd=0)`; our MQDescriptor+FmqConfig match
  byte-for-byte; `clienttest.cpp` proved our client binder is callable from a second
  process). Yet `availableToRead=0`, `infoCalls=0` — the HAL's closed
  `SensorClientManager<ImuData>` simply never routes IMU to our queue. **Option 2 also
  unlocks the cameras** (same client mechanism, `ICameraProvider`) and gives processed,
  clock-synced IMU for free.

Decision taken: **Option 1 (syncboss-direct)** as the ship path; live-HAL memory
inspection as the shared next step (it cracks the option-2 gate AND yields the
calibration + timestamp-translation constants option 1 needs).

## Device state (as left — VERIFY before assuming)

- **SELinux = Permissive** (`setenforce 0` was run during inspection; restore with
  `su -c setenforce 1` when done — or keep permissive while iterating).
- HAL `vendor.oculus.hardware.sensors@1.0-service` = **pid 18720** (restarts change pid!).
  `trackingservice` = **pid 18709**.
- No yama on this kernel; ptrace gated only by SELinux. Root via `su -c` (Magisk,
  context `u:r:magisk:s0`). adb shell is uid 2000 (shell).
- **Restart-cascade hazard**: any stop/kill of `sensors@1.0-service` restart-cascades and
  can SIGKILL the adb shell. Read-only `/proc/pid/mem` inspection does NOT stop the target.
- Maps snapshots (host): `/tmp/hal18720.maps`, `/tmp/track18709.maps` (re-dump if pids changed).

## Tool built this session: `mempeek` (no debugger, read-only)

- Source: `tools/mempeek/mempeek.c` (pure C, NDK aarch64 PIE, no deps beyond bionic).
- On device at `/data/local/tmp/mempeek` (mode 755). Rebuild:
  `NDK/tools.../aarch64-linux-android29-clang -O1 -fPIE -pie -o mempeek mempeek.c`
  (absolute NDK path: `tools/android-ndk-r27c/toolchains/llvm/prebuilt/linux-x86_64/bin/`).
- Commands (run as root): `mempeek <pid> maps | dump <addr> <len> | read <addr> <nwords> |
  scan <hexbytes> [maxhits] | scanptr <value> [maxhits]`.
  scan/scanptr walk every readable region, print hit + 32-byte context. 64 KiB chunked
  with plen-1 overlap.

## Service binary symbols — NEW ASSET (use these)

- `recon/hal-A-2026-08-31/vendor.oculus.hardware.sensors@1.0-service`:
  **`.gnu_debugdata` at file offset 0x96148, size 0x63fc, is a 7z archive** (7z magic).
  Extract → **full unstripped ELF** (270,696 B, "not stripped").
- Done: `work/svc-dbgdata/svc-debugdata` (ELF), `work/svc-dbgdata/syms.txt`
  (324 `T` symbols, "addr name" lines). Regenerate:
  ```
  python3 -c "open('work/svc-dbgdata/svc-debugdata.7z','wb').write(open('<svc>','rb').read()[0x96148:0x96148+0x63fc])"
  7z x -y -owork/svc-dbgdata work/svc-dbgdata/svc-debugdata.7z
  nm -C work/svc-dbgdata/svc-debugdata | grep ' T ' > work/svc-dbgdata/syms.txt
  ```
- Key addresses (file offsets — add runtime load base from `/proc/<pid>/maps`, which is
  the first `r--p` mapping of the .so at `6519c3a000` in the current maps; base =
  runtime_addr_of_first_mapping - 0):
  - `SensorTraits<Imu>::enableSensor(void*,float)` **0x4fdcc** ← THE 1 kHz IMU enable
  - `SensorTraits<Imu>::disableSensor(void*)` 0x4fe10
  - `SyncbossEventHandler::enableSensor(SensorType,…)` 0x55cc8 / disable 0x561e8
  - `CameraStream::prepareStream` 0x39134, `streamControl` 0x39c8c, `startStream` 0x385f8
  - `SyncbossEventHandler::SyncbossEventHandler` 0x55b40, `getSession` 0x55c10
  - `VsyncClock::VsyncClock` 0x816d0, `MontereyHostTimeSource::getLatestTimestamp` 0x54d88
- Many impl symbols are inlined/COMDAT-folded (`SensorClientManager<ImuData>::*`,
  `Imu::prepareStream`) — not in this table. `nm` the full ELF for data symbols too
  (globals/statics) — not yet mined.

## FMQ / ring findings so far (this session)

> ⚠️ PARTIALLY SUPERSEDED — the record-shape bullets below used a wrong 48-byte stride.
> See "RETRY SESSION ... FMQ layout — REVISED" below for the corrected 16-byte header
> and the open stride question. The addresses/counters remain valid.

- HAL (18720) maps only three non-4K `MessageQueue` ashmems: 8192 B @0x789c372000,
  16384 B @0x789c3ef000, 24576 B @0x789c3f4000 — **all headers all-zero** → stale/unused.
  **No `openvr-imu-evflag` mapping in HAL** → our last client's evflag is not held.
- trackingservice (18709): ~90 4K MQ regions, all headers zero, **except two live rings**:
  - **0x746258e000** (mapped again at 0x746258f000/0x7462590000): header `0x000154f0`
    (87,280) at +0x00 AND +0x08, then 0x30 (48-byte) records:
    `{u64 ctrA (~0x119b_abfd_40), u64 ts1 (~0x2d1b_b425_47e2), u64 ts2 (ts1+~0x21f82),
    u32 0 + u32 float-ish (0x38ab4b8f≈2.94), u64 (2 floats ~1.07/1.48), u64 ctrB
    (~0x119d_b1db_78)}`. ts1 delta ≈ 0x21f82 ≈ 1.397 M ticks/record; ctrB delta ≈ 0x1000
    /record.
  - **0x7462591000** (+0x7462592000/0x7462593000): counter `0x47e380` (4,689,600),
    same 0x30 record shape.
  - **Both rings FROZEN at sampling time** (1.5 s apart, identical) — headset was set
    down (no active tracking), consistent with "IMU only streams while worn/active".
- **UNRESOLVED (the retry point):** → MOSTLY CLOSED in RETRY SESSION 2 above
  (B = 1 kHz IMU, A = 30 Hz gyro, strides 64 B / 40 B, ts in ns). Remaining: ClientInfo
  hunt, float units, ts2 semantics, ring capacity.
  1. Which ring is the 1 kHz IMU (A: 87,280 writes ≈ 87 s @1 kHz; B: 4,689,600 ≈
     78 min @1 kHz, or ~43 h @30 Hz)? Check device uptime + process start times to
     bracket; or wear the headset and watch writeIndex advance (1 kHz → +60,000/min;
     30 Hz → +1,800/min).
  2. Decode the 48-byte record: which field is accel/gyro (3 axes × 2 = 6 floats? the
     0x28–0x38 region has 12 float-ish 32-bit values) and which are timestamps
     (nRF syncboss clock vs host clock → that's the clock-translation piece option 1 needs).
  3. Find the `SensorClientManager<ImuData>` client list in HAL heap: `mempeek 18720
     scanptr <ring base 0x746258e000>` → ClientInfo(s); compare with any entry for our
     client (or confirm absence). ClientInfo = `{MessageQueue<T, sync>, EventFlag*,
     hidl_handle, ISensorClient, uint, uint}`.
  4. If our client was registered, diff its ClientInfo fields vs trackingservice's —
     that diff IS the gate.

## Other proven facts (from notes 06/07 — don't re-derive)

- IMU = ICM-20602, HAL `getProperties` serves factory + online calib JSON inline, rate 1000.
- `sizeof(ImuData) = 64` (from `libvrsensors-hidlwrapper.so` ctor: `lsl x1,x21,#6`).
- `FmqConfig` = `{hidl_handle eventFlag; uint32 bitA; uint32 bitB}` (24 B); real client:
  bitA=0x8000, bitB=0x1; evflag = ashmem(4B) + `ashmem_set_prot_region(fd,RW)` +
  `native_handle_create(1,0)`.
- MQDescriptor: grantorCount=3, quantum=64, flags=1; grantor bytes `00..|08..|08.. 08..`.
- Real recipe: `prepareStream` → `streamControl(cmd=0)`; the real client never calls
  streamControl for IMU beyond that; `getSensorClientInfo` is identity-only.
- Build recipe for HIDL clients: `tools/hal_stream/build.sh` (NDK r27c, AOSP android-10
  headers in `tools/aosp-headers/inc`, patched `__config_site` `__ndk1`→`__1` at
  `/tmp/cxxpatch/c++/v1`, generated bindings in `tools/hidl-build/gen`, `-fno-rtti`,
  `-Wl,-z,muldefs`, link pulled devlibs in `recon/hal-A-2026-08-31/devlibs/`).
- Syncboss nodes: `/dev/syncboss0`, `_control0`, `_stream0`, `_powerstate0`
  (`system:system 0664`), driver `oculus_syncboss` on spi12.0; MCU fw
  `/vendor/firmware/syncboss.bin` (147,920 B, runs on the nRF, keep loading it).
- Cameras: 4× OV7251, QC V4L2 (`/dev/video3` msm-sensor etc.) driven inside the same
  HAL (`ICameraProvider`); frame pixels ride ION/dmabuf, FMQ carries metadata
  (`FrameSet` = `gsl::span<Frame>` + `ImageBufferHandle`).

## RETRY SESSION 2 (this session, 17:45–18:10) — BREAKTHROUGHS

### STRIDE + RATES RESOLVED (open questions 1, 2, 3 CLOSED)

- **Ring B (0x7462591000) = THE 1 kHz IMU ring.** Record stride = **64 B = sizeof(ImuData)**
  (confirms note-06). ts1 delta = 1,006,000 ticks = **1.006 ms → 994 Hz ≈ 1 kHz**.
  Counter 4,689,600 @994 Hz = **78.4 min** of streaming (process alive 4.5 h → worn ~78 min
  before set-down). ✓ self-consistent.
- **Ring A (0x746258e000) = 30 Hz gyro ring.** Record stride = **40 B**
  `{u64 ts1, u64 ts2, float[6]}` (no ctrA field). ts1 delta = 33,933,000 ticks = 33.93 ms →
  **29.5 Hz ≈ 30 Hz**. Floats ≈ 0.00005–0.00008 (gyro bias at rest, rad/s).
  Counter 87,280 @29.5 Hz = 49 min. ✓ self-consistent.
- **Timestamp unit = NANOSECONDS in both FMQ rings** (not the nRF µs tick). The syncboss
  30 Hz broadcast ts32 IS the 1 MHz nRF clock (33,333 ticks/33.3 ms). FMQ ts1 = ns wall
  (~49,597 s ≈ 13.8 h — consistent with nRF uptime; device uptime 18.2 h, nRF booted
  slightly after or has its own epoch). ctrA in ring B is a **µs-domain counter**
  (delta ≈ 0xF59B0 = 1,004,240 per 1.006 ms record ≈ 1 MHz).
- **Ring B 64 B record layout (CONFIRMED, 16 consecutive records decode clean):**
  ```
  +0x00  u64 ctrA      (µs counter, delta ~0xF59B0/record)
  +0x08  u64 ts1       (ns — the IMU sample timestamp, 994 Hz cadence)
  +0x10  u64 ts2       (ns — ts1 + 2.2–3.2 ms, semantics TBD: next-frame? vsync-aligned?)
  +0x18  u64 pad       (0xffffffffffffffff, always)
  +0x20  float[6]      (v1=[30.2,-7.0,-2.45] |v1|=31.1 ; v2=[6.30,0.055,0.012] |v2|=6.30)
  +0x38  u32 tail      (float-ish 0x3c2e886e etc, changes slowly)
  +0x3c  u32 0
  ```
- **Ring A 40 B record:** `{u64 ts1 (ns), u64 ts2 (ns, ts1+2.2–3.2 ms), float[6]}`. No
  ctrA, no pad, no tail.
- **Ring A has a legacy 48 B block at +0x10** (between 16 B header and 40 B records at
  +0x40): `{u64 ctrA, u64 ts1, u64 ts2, u32 0, u32 float, float×4, u64}` — an OLD record
  format, frozen (ts ~49597009 s, older than the 40 B records). Suggests ring A was
  re-formatted when the client (re)attached.
- **Both rings: 16 B header** `[u32 counter, u32 0, u32 counter(dup), u32 0]`;
  data at +0x10 (ring B) or +0x40 (ring A, after legacy block).

### ANOMALIES (noted, unexplained)

- **Out-of-order record in ring B**: 1 record mid-buffer with ts1 31.19 ms EARLIER than its
  neighbors (r14: ts=49597.178 s vs neighbors 49597.20–.21 s), ctrA 19.4 ms behind.
  Possible duplicate/buffering glitch, or the ring is a fixed-size circular buffer with
  one stale slot. Ring is 12 K (3 pages); at 64 B/record the capacity is ~192 records
  = 192 ms of IMU. (Need exact capacity: is it 3 independent 4 K ashmems or one 12 K
  object? Maps show 3 separate 4 K `MessageQueue (deleted)` entries — see below.)
- **Float units UNRESOLVED**: ring B v1 magnitude 31.1 ≠ 9.8 m/s², v2 6.30. If v1 is
  accel, unit scale ≈ 31.1/9.8 = 3.17 (weird). Maybe v1 is in a different unit (1/32 g?
  fixed-point?) or v1 isn't accel. Ring A gyro floats ~0.00005 rad/s at rest are sane.
  Calib JSON (exports/calibration-2026-08-30/calibration/imu_calibration.json) has
  3×3 RectificationMatrix + Offset (accel offset ~ -0.01..-0.14, gyro offset ~0.06 —
  looks SI). Need the raw→SI scale; check syncboss raw packet payload or HAL code.
- **ts2 - ts1 ≈ 2.2–3.2 ms** in BOTH rings — same offset, unknown semantics.

### ASHMEM TOPOLOGY (the stream architecture, from maps names)

trackingservice maps (addresses, all `/dev/ashmem/... (deleted)`):
```
0x7462581000  ConstellationTrackingStream (4 K)
0x7462582000  CalibrationData             (48 K = 12 pages)
0x746258e000  MessageQueue ×3             ← RING A (12 K, 30 Hz gyro)
0x7462591000  MessageQueue ×3             ← RING B (12 K, 1 kHz IMU)
0x7462594000  MotionSensorCompositeStream (4 K)  ← first u64 = 0x18000 = 98,304 (size? cap?)
0x7462595000  MessageQueue ×3             (12 K)
0x7462598000  EvFlagHandle0               (4 K)
0x7462599000  MessageQueue ×3             (12 K)
```
→ The **IMU "MotionSensor" client block = ring A + ring B + MotionSensorCompositeStream
+ 2 more MessageQueue×3 groups + EvFlagHandle0**. The `MotionSensorCompositeStream` 4 K
page (first 8 B = 0x00018000 = 98,304) is the composite-stream descriptor; rest zero.
HAL maps the SAME objects at 0x789c331000/0x789c335000 (2 pages, non-contiguous).
- trackingservice ImageBuffer maps are `r--s` (read-only), HAL's are `rw-s` → pixels
  written by HAL, read by trackingservice. Same model for FMQs (HAL writes, client reads).
- `CalibrationData` (48 K) is mapped ONLY in trackingservice (not in HAL maps) — the
  calib handle region. `ConstellationTrackingStream` in both.
- Named ashmems present in HAL: ExposureControlStream×2, ObjectTrackingStream×2,
  HandTrackingStream×2, ConstellationTrackingStream×1, **MotionSensorCompositeStream×2**,
  IOTcompositeStream×1, ControllerTrackingCompositeStream×10, ControllerManagement×1,
  EvFlagHandle0–7×8, MessageQueue×91, ImageBuffer×64.

### STILL OPEN (retry point)

1. **ClientInfo hunt in HAL heap** (the actual gate): `mempeek 18720 scanptr 0x7462591000`
   and `scanptr 0x746258e000` — but note HAL maps these ashmems at ITS OWN addresses
   (0x789c... range), so a scan for the trackingservice-side VA (0x74625...) will MISS.
   Must first find the HAL-side VAs of ring A/B (match by content: dump each 12 K
   `MessageQueue` in HAL maps and find the one whose header counter = 0x47e380 / 0x000154f0,
   or scanptr the HAL-side address once found). Then scanptr THAT address in HAL heap →
   ClientInfo(s).
   Practical shortcut: `mempeek 18720 scan 80 e3 47 00 00 00 00 00 80 e3 47 00` (ring B's
   exact 16 B header) → finds the HAL-side copy of ring B directly.
2. **Float field map + units** (open): which 3 = accel, which 3 = gyro; raw unit scale
   (|v1|=31.1 at rest). Get syncboss raw IMU packet payload (the 1 kHz enable via
   syncboss_control0) and compare, or RE the HAL's ImuData fill path.
3. **ts2 semantics** (ts1 + 2.2–3.2 ms in both rings).
4. **Ring capacity**: 3×4 K separate ashmems vs one 12 K; exact element count
   (ring B: 12 K/64 B ≈ 192 records ≈ 192 ms @1 kHz).
5. **Our client's evflag still not in HAL maps** → prepareStream likely still rejected.

### Commands used (verbatim, for replay)

- `adb shell 'su -c "/data/local/tmp/mempeek 18709 dump 0x7462591010 1024"'` → ring B 1 K
- `adb shell 'su -c "/data/local/tmp/mempeek 18709 dump 0x746258e010 1024"'` → ring A 1 K
- `adb shell 'su -c "head -c 200 /dev/syncboss_stream0 | xxd"'` → live broadcast ts32s
- `adb shell 'su -c "ps -p 18709 -o pid,etime,time,comm"'` → process age/CPU
- ring dumps saved host-side: /tmp/ringB_1k.txt, /tmp/ringA_1k.txt, /tmp/ringB_base.txt,
  /tmp/ringA_base.txt

## RETRY SESSION 1 (17:20–17:45) — findings (superseded where conflicting)

### New hard facts

- **Device**: SELinux Permissive, uptime 18.2 h (65,651 s). **trackingservice (18709) AND HAL
  (18720) both restarted at 12:58** (elapsed 4:31:49) — NOT alive since boot. Pids still valid.
- **trackingservice CPU = only 6 s over 4.5 h** → idle most of the time (headset on desk;
  rings freeze when unworn). HAL CPU = 7 s.
- **Syncboss broadcast rate = 30 Hz, nRF tick = 1 MHz exactly**: consecutive broadcast
  ts32 deltas are 33,333 ticks per 33.3 ms packet → **1 tick = 1 µs**. (ts32 = low 32 bits
  of the 1 MHz tick; high 32 bits live in the 48-bit FMQ timestamps.)
- **trackingservice holds NO syncboss fds** — all sensor data arrives via the HAL over FMQ.
  Its fds are binder/hwbinder + dozens of ashmem only.
- **HAL-only 24K ring (0x789c3f4000) is all zeros** → stale/never used. The ONLY live FMQs
  system-wide are trackingservice's ring A (0x746258e000, ctr 87,280) and ring B
  (0x7462591000, ctr 4,689,600). The 8K/16K HAL rings also zero.

### FMQ layout — REVISED (corrects earlier notes)

- **Header = 16 bytes**: `[u32 counter, u32 0, u32 counter(dup), u32 0]`. Ring A header:
  `f0 54 01 00 00 00 00 00 f0 54 01 00 00 00 00 00` = counter 87,280. Ring B: counter
  4,689,600. (Not the 8-byte AOSP MessageQueueHeader — this is a custom/duplicated layout,
  or the counter is readIdx=writeIdx with a second copy.)
- **Records start at offset 0x10.** Record stride is NOT 48 — my 48-byte decode fell apart
  at record 1 (fields went to garbage). The per-record layout I was assuming was WRONG:
  - What looks like `{u64 ctrA, u64 ts1, u64 ts2, u32, u32, f, f, f, f, u64}` at 0x10 is
    really probably `{u64 ts (48-bit value in low bytes), ...}` with the true record being
    a different size. Ring A's "t2-t1" deltas (2.2–2.4 s @1 MHz) and ring B's (2.4 s) look
    like the ts of the NEXT sample, not a second field.
  - Ring A floats at +0x1c/+0x24 region ≈ **tiny (0.0001)** — consistent with gyro
    (rad/s or dps at rest). Ring B floats ≈ **30.2 / -7.0 / -2.45 / 6.3** — one ~30 value
    looks like a 1-g accel component in some unit, or a mag component; NOT quiet-rest values.
  - **STRIDE STILL UNRESOLVED** — the immediate next micro-task (see plan below).

### Ring identity — PARTIALLY resolved

- Ring A counter 87,280: if 1 kHz → 87 s of streaming; if 300 Hz → 4.9 min; if 30 Hz → 48 min.
- Ring B counter 4,689,600: if 1 kHz → 78 min; if 300 Hz → 26 min; if 100 Hz → 78 min;
  if 30 Hz → 43 h (impossible: process alive 4.5 h). So **ring B ∈ {1 kHz (78 min),
  100 Hz (78 min), 300 Hz (26 min)}** — cannot distinguish without live rate sampling.
- **Timestamp-gap cross-check FAILED to disambiguate**: ring B last ts1 = 0x2dbf41ea72,
  current syncboss ts32 = 0xe29ee77a. Elapsed lower-32 = 0.16–0.86 h depending on which
  record is "last" (stride uncertainty) and whether 32-bit wrapped (wrap = every 49.7 days,
  so no wrap in 18 h uptime — gap IS real). 0.16 h = 10 min: ring B's last sample was ~10
  min before my sample → **headset was worn until ~17:15 and set down** — plausible and
  consistent with ring B being the high-rate IMU ring (it stops when unworn).
- Both rings frozen during my 20 s sampling window (headset on desk) — confirmed.

### Open questions (retry point)

1. **Record stride**: dump ring B at 0x10 for 0x400 bytes and find the true stride by
   looking for the repeating `00 00 00 00 00` low-32 pattern / 48-bit timestamp cadence.
   (At 1 MHz the ts deltas should be constant = stride_ticks = 1e6/rate; the stride
   itself is probably 32 or 64 bytes, and my 48 was the bug.)
2. **Which ring is the 1 kHz IMU**: A (87,280 → 87 s @1 kHz) vs B (4,689,600 → 78 min @1 kHz).
   Wear test: sample both counters for 10 s → +10,000 = 1 kHz, +300 = 30 Hz, +1,000 = 100 Hz.
   (The 10-min-stale ring B gap strongly hints B = IMU, A = lower-rate sensor or a short
   burst buffer.)
3. **Ring A identity**: tiny floats (0.0001) = gyro at rest? Or is A a 30 Hz gyro/mag ring
   and B the 1 kHz fused IMU?
4. **Float field map**: which 3 are accel, which 3 gyro (or where the mag lives), units,
   and the two 48-bit timestamps (nRF µs tick vs host clock) — that's the clock-translation
   + calibration-constant extraction Option 1 needs.
5. **ClientInfo hunt in HAL heap**: `mempeek 18720 scanptr 0x746258e000` and
   `scanptr 0x7462591000` → find ClientInfo(s); diff against our client's (or confirm
   absence) → the gate.
6. **Our client's evflag ashmem is NOT mapped in HAL** (no `openvr-imu-evflag` in
   18720 maps) → our last prepareStream's descriptor may have been silently rejected.

## Immediate retry plan (where we stopped — stride/rates DONE, now the HAL heap)

1. `adb devices` → confirm; `su -c 'getenforce'` (expect Permissive); re-verify pids
   (`ps -A | grep -E 'sensors@1.0|trackingservice'`), re-dump maps if changed.
2. **Find HAL-side copies of ring A/B**: `mempeek 18720 scan 80 e3 47 00 00 00 00 00
   80 e3 47 00 00 00 00 00` (ring B's exact 16 B header) and same with
   `f0 54 01 00 00 00 00 00 f0 54 01 00 00 00 00 00` (ring A). → HAL-side VAs of the rings.
   (scanptr on the trackingservice-side VAs would MISS — HAL maps the ashmems at its own
   0x789c... addresses.)
3. **ClientInfo hunt**: `mempeek 18720 scanptr <HAL-side ring B base>` (and ring A) →
   hits in HAL heap = ClientInfo(s) or SensorClientManager nodes referencing the queues.
   Dump 0x100 B around each hit. Look for the ISensorClient binder pointer (trackingservice's
   vs ours) + rate/enabled flags. Diff trackingservice's entry vs any entry for our client
   (or confirm ours is absent) → THE GATE.
4. If the gate is a flag/rate field → replicate in our client or set via breakpoint
   (gdbserver attach to 18720, microsecond stops — restart-cascade hazard applies).
   If our ClientInfo is absent entirely → prepareStream silently rejected (consistent with
   no `openvr-imu-evflag` in HAL maps) → re-examine descriptor acceptance.
5. **Float units + field map** (parallel): enable 1 kHz IMU on `/dev/syncboss_control0`
   (Option 1 wire RE) and compare raw payload vs ring B floats; or RE the HAL's ImuData
   fill path (symbols in work/svc-dbgdata/syms.txt). Target: accel/gyro assignment +
   raw→SI scale (|v1|=31.1 at rest is the anchor: if accel, scale = 31.1/9.8).
6. **ts2 semantics** (ts1+2.2–3.2 ms in both rings) + ring capacity (12 K = 192 rec @1 kHz?)
   → check via maps (3 separate 4 K ashmems) or HAL code.
7. When done: `su -c setenforce 1` (unless iterating), write findings to `notes/07-*.md`
   (append a new section), and update this file's "STILL OPEN" list.

## RETRY SESSION 3 (Claude, lldb live-debug on HAL 18720) — SENSE-CHECK + CORRECTIONS

Method note: used NDK aarch64 `lldb-server gdbserver --attach 18720` + host lldb (brew,
`/home/linuxbrew/.linuxbrew/bin/lldb`; the NDK host lldb needs libpython3.11 → unusable).
Attach STOPS the HAL only for the read then `detach` (state returns to S) — no restart, no
cascade. Verified safe across ~6 attach cycles.

### [VERIFIED-CLAUDE] hard facts (directly observed this session)

- **Imu impl singleton @ `0x789c683540`** (for HAL pid 18720). `SensorClientManager<ImuData>`
  is embedded at **impl+0x28**; its client container = `std::vector<ClientInfo>` with
  **begin ptr @ SCM+0x70, end ptr @ SCM+0x78**, **ClientInfo stride = 0x98 (152 B)**.
- **Vtable slots (IImu impl):** getProperties = `-service+0x4ec70`, prepareStream = `+0x4ef90`
  (real body `+0x4efc4`), streamControl = `+0x4f2d8` (body `+0x4f30c`). Derived from the
  `_hidl_streamControl` call `ldr x9,[x8,#0x78]; blr x9` (vtable slot 15) + the vtable dump.
- **ClientInfo layout** (from the `...ClientInfo` allocator::construct symbol + memory):
  `+0x00` state word; `+0x08/+0x18/+0x20` FMQ/EventFlag/handle ptrs; `+0x28` **callingPid
  (int)**; `+0x30` **ISensorClient sp**; `+0x38` = `0x0000000100008000` (**bitA=0x8000,
  bitB=0x1**); `+0x40..` per-substream stat triples `{rate 0x77359400, ts, count}`.
- **prepareStream (body 0x4efc4):** calls `getCallingPid()`, `createEventFlagFromHandle()`,
  builds FMQ from our descriptor, **UNCONDITIONALLY `emplace_back` a ClientInfo** — NO
  uid/pid rejection. Two error logs exist (`SensorService` tag): `"SCM: Client %d: Failed to
  create stream"` and `"SCM: Failed to obtain event flag for client: %d"`. **Neither fired**
  for our client (logcat -s SensorService during our run).
- **streamControl (body 0x4f30c):** `getCallingPid()`, finds client by `interfacesEqual`,
  branches: `cmd==2`→STOP, `cmd==1`→(branch), **`cmd==0`→START** (sets `state[+0]=1`, counts
  started clients, calls the enable vpath). `"SCM: Received unknown stream command"` fires
  ONLY for cmd∉{0,1,2}. → **cmd=0 is a real START, not a no-op** (corrects earlier "unknown"
  reading). "SyncbossEventHandler: starting all streams" DID fire when our client connected.
- **[CORRECTION to qwen retry-pt #5 / "prepareStream rejected"]** — **OUR CLIENT IS
  REGISTERED.** I directly saw TWO ClientInfo entries live: `#0 pid=18709` (trackingservice)
  AND `#1 pid=2727` (our `hal_stream4`), while our client was connected. qwen's "no
  openvr-imu-evflag in HAL maps / ClientInfo absent" was a **false negative from sampling with
  our client disconnected** — the evflag mapping + ClientInfo exist ONLY while our client
  process is alive (our client self-exits after its ~120 s poll loop, so snapshots taken later
  see count=1). **Do not pursue "descriptor silently rejected" — it is accepted.**
- **ClientInfo diff (our pid 2727 vs TS 18709, both captured live, same instant):**
  `+0x00`: ours=`1`, TS=`0`.  `+0x80`: ours=`0x789b3c5f01` (ptr-ish), TS=`1`.
  Everything else identical (FMQ ptr, EventFlag, bits 0x8000/0x1, rate 0x77359400, client sp).
- **Ring B (0x7462591000) counter advanced 4,689,600 → 4,711,296** between qwen's session and
  mine → corroborates B is the live IMU ring. Within each of my sampling windows the rings
  were frozen.

### [CONFOUND / do-not-over-conclude] flags

- **The `+0x00` diff is almost certainly a START/STOP artifact, NOT the gate.** cmd=0=START
  sets state=1; our client called streamControl(0) → ours=1. TS currently shows `0`
  (stopped/idle). But TS is the WORKING client — so "state==1 ⇒ delivered" is contradicted by
  TS=0. A clean diff requires **both clients STARTED at the same time** (see headset flag).
- **[UNVERIFIED — user flag] "IMU streams only while worn" was NEVER tested.** The user
  confirms qwen never prompted them to put the headset on/off. So the frozen rings are NOT
  established to be caused by an unworn headset — that is qwen's *assumption*. Freeze cause is
  currently unknown (unworn, OR trackingservice idle for another reason, OR stream genuinely
  stopped). Needs a deliberate worn/unworn A/B (user action) to validate.
- **Because the rings are frozen NOW, no client receives IMU at all** → any live "does our
  client get data" test is INVALID until the IMU is actually streaming. The only evidence the
  gate exists with IMU live is the earlier pre-compaction worn test ("its on", TS ~65% CPU,
  our client still 0) — treat as reported, RE-VERIFY.
- Our test client self-exits (120 s loop) → ClientInfo vanishes. Need a **stay-resident**
  client for stable inspection/comparison.

### [QWEN-UNVERIFIED by Claude — return to verify before relying on these]

- **FMQ ring record DECODE** (RETRY SESSION 2): ring B 64 B `{ctrA, ts1, ts2, pad 0xff.., 
  float[6], tail}`; ring A 40 B; **timestamps = ns**; rates 994 Hz / 29.5 Hz; float units
  (|v1|=31.1). — NOT independently checked by me. This is the IMU data-format claim option 1
  depends on; verify by decoding rings myself + cross-checking vs syncboss raw.
- **nRF tick = 1 MHz**, broadcast = 30 Hz (I only roughly confirmed ~30 Hz packet rate).
- **Ashmem topology / stream names** (MotionSensorCompositeStream, ring A/B naming), device
  **uptime 18.2 h**, both procs **restarted 12:58**, **trackingservice holds no syncboss fds**
  — all qwen, not re-checked.
- **Symbol addresses**: I independently confirmed `SensorTraits<Imu>::enableSensor` = 0x4fdcc.
  Others (CameraStream 0x39134, VsyncClock 0x816d0, MontereyHostTimeSource::getLatestTimestamp
  0x54d88) NOT re-checked.
- `.gnu_debugdata`: I extracted via `xz -d` → 270,696 B ELF (matches qwen's size); qwen says
  "7z magic". Same artifact, tool discrepancy noted.

### [OPEN — the real gate is still unproven]

The gate is NOT descriptor rejection (disproven). Candidates remaining: (a) a per-client
delivery predicate in the (inlined) writer loop on the streaming thread; (b) the `+0x80`
difference; (c) something only visible with IMU live + both clients started. Next: either
find/observe the writer loop's per-client condition, OR do a clean worn A/B with a
stay-resident client and re-diff.

## RETRY SESSION 3b (Claude) — OPTION 1 IMU DECODED FROM KERNEL [VERIFIED-CLAUDE]

**The 1 kHz IMU is NOT gated on wearing the headset.** With a HAL client connected that
requests streams (our `hal_stream4`, which fires "SyncbossEventHandler: starting all streams"),
`/dev/syncboss_stream0` immediately carries **type-0x50 IMU packets**. With no client, only the
30 Hz type-0xe0 broadcast flows. So qwen's "IMU only streams while worn" is FALSE — it streams
whenever any client requests it (and we can be that client, or send the enable on control0).

**Syncboss stream framing:** `01 03 00 <TYPE> 00 <LEN> <payload[LEN]>`
- `TYPE=0xe0, LEN=0x0e(14)` = 30 Hz always-on broadcast (the old finding).
- `TYPE=0x50, LEN=0x24(36)` = **1 kHz IMU**. Payload (VERIFIED by decoding 64 live packets):
  ```
  +0x00 u32 ts_us    nRF 1 MHz clock; Δ=1006–1007 us/packet → 994 Hz ≈ 1 kHz ✓
  +0x04 u32 id = 5   (constant; IMU sensor id)
  +0x08 f32 accel_x  units = g
  +0x0c f32 accel_y  |accel| = 1.005–1.007 g at rest  ✓✓ (gravity)
  +0x10 f32 accel_z
  +0x14 f32 gyro_x   ~deg/s (≈3 deg/s magnitude at rest = uncalib bias)
  +0x18 f32 gyro_y
  +0x1c f32 gyro_z
  +0x20 f32 temp     ≈36.5 C, stable
  ```
  Sample (at rest): accel=(-0.903,-0.396,0.193) g, gyro=(3.24,0.73,0.55), temp=36.47.
- **nRF emits IMU already in physical units (g, deg/s, C)** → Option 1 needs NO raw→SI
  calibration scale; at most subtract the small gyro bias (factory calib JSON) + axis-map into
  the Basalt/Kalibr frame. This is far simpler than expected.

### [CORRECTION to qwen ring-B float analysis]
qwen's ring B `|v1|=31.1` (RETRY 2) was the HAL's RE-PROCESSED `ImuData` (different
units/format), NOT the sensor. The RAW syncboss IMU (above) is the clean source. Ring-A/B
decode in RETRY 2 is HAL-side and should be treated as unverified for our purposes — the raw
stream supersedes it for Option 1.

### [still to verify for a self-contained Option 1]
1. Enable IMU **without** our HAL client: RE the `syncboss_control0` write that turns on
   type-0x50 (so we don't depend on running a HAL client). Trace the HAL's write to
   `/dev/syncboss_control0` (ftrace/strace-on-attach) or RE `SyncbossEventHandler::enableSensor`
   (0x55cc8) + `SensorTraits<Imu>::enableSensor` (0x4fdcc).
2. Confirm gyro units (deg/s vs rad/s) and exact axis order vs the calibration frame.
3. ts is 32-bit us (wraps ~71 min) — handle wrap for fusion.
4. Camera still needs the HAL path (unchanged).

## RETRY SESSION 4 (Claude) — CAMERA FEASIBILITY PROBE [VERIFIED-CLAUDE]

Question: can we get camera frames WITHOUT beating the HAL (the IMU "go under the HAL" pattern)?

**V4L2 topology (msm8998 MSM camera_v2):** `/dev/media0-4`, `/dev/v4l-subdev0-13`
(3× msm_csiphy, 4× msm_csid, 2× vfe, msm_ispif, msm_buf_mngr, cpp, msm_sensor_init,
msm_camera_laser_led), `/dev/video0=msm-config`, `video1=jpegdma`, `video2=sde_rotator`,
**`video3-6 = msm-sensor` (the 4 OV7251 tracking cams)**, video32/33 aux.

- **[BLOCKED] Independent parallel V4L2 capture is NOT possible.** The sensors HAL (18720)
  **exclusively holds `/dev/video0` (msm-config)** — the single session hub for the whole MSM
  camera pipeline (frames don't come out of the sensor nodes via VIDIOC_DQBUF; they flow through
  msm-config's proprietary session into ION buffers). No separate camera daemon; the HAL drives
  the OV7251s directly via `libqcameraoculushal.so`. Only pid 18714 (mrsystemservice) also holds
  a node (`/dev/video33`, aux).
- **[FEASIBLE, moderate eng] Frames ARE shared and locatable.** Camera frames land in ION
  dmabuf, wrapped by `ImageBuffer` ashmem handles shared HAL(rw-s)↔trackingservice(r--s)
  (SAME inodes, e.g. 951981/951982). TS maps **512 ImageBuffer** handles + a large
  `anon_inode:dmabuf` pool.
  - **ImageBuffer handle decoded** (dump @ TS 0x74600c5000): `{u64 0x0f, u32 width=0x280=640,
    u32 height=0x1e1=481, u32 1, u32 1, u64 0x20033, u64 size=0x4c000=311296, u32 stride=640}`
    → **640×480 8-bit mono OV7251 frame**, 311,296 B.
  - **Pixel buffers = `anon_inode:dmabuf` regions each exactly 0x4c000 (311296 B) = one frame**,
    mapped rw-s (inode 17486), a whole pool. These are the raw camera frames.
- **[GOTCHA] dmabuf pixels are NOT readable via `/proc/<pid>/mem`** (mempeek → I/O error). ION
  dmabuf needs CPU access via: re-open the dmabuf fd through `/proc/18709/fd/<N>`, `mmap` it,
  `DMA_BUF_IOCTL_SYNC` START/END, then read. (The ashmem ImageBuffer *handles* read fine; only
  the dmabuf *pixels* need this.) Mechanism not yet proven on-device — NEXT STEP.
- **Streaming trigger unknown** (same shape as IMU): cameras likely only stream when a
  tracking session/app is active. Buffers may be stale when idle.

### Camera routes (ranked)
1. **Leech TS dmabufs read-only** (fd re-open + DMA_BUF sync) + read the FrameSet FMQ for the
   current-buffer index. Reuses the IMU "read shared memory" pattern. Needs streaming active.
2. **Build a camera HIDL client** like the IMU one (`ICameraProvider`/`CameraStream::prepareStream`
   @ svc+0x39134 / streamControl @+0x39c8c) — same SensorClientManager gate we hit for IMU
   (delivery-loop condition still unproven). If cracked, delivers FrameSet + buffer handles to us.
3. Drive msm-config ourselves — reimplement QC msm_camera session; heavy; conflicts w/ HAL.

## RETRY SESSION 4b (Claude) — CAMERA dmabuf READ: EXTERNAL LEECH IS IMPOSSIBLE [VERIFIED-CLAUDE]

Tested the "leech TS frame dmabufs as root" route (RETRY 4 route 1). **It does NOT work:**
- **fd re-open blocked:** `open("/proc/18709/fd/<N>")` on an `anon_inode:dmabuf` → **ENXIO**
  ("No such device"). This 4.4 kernel won't reopen dmabuf via procfs; `pidfd_getfd` (5.6+) absent.
  Built `tools/dmabuf_read/dmabuf_read.c` (on device `/data/local/tmp/dmabuf_read`) — 1024
  dmabufs in TS, 0 openable.
- **external memory read blocked:** the mapped dmabuf region (e.g. TS 0x74191b3000, 0x4c000 B)
  has **`VmFlags: rd wr sh mr mw me ms pf io de dd`** → **VM_PFNMAP | VM_IO**, Rss=0/Pss=0. All
  external readers (`/proc/pid/mem`, `process_vm_readv`, ptrace PEEK, lldb) use
  `access_process_vm`/get_user_pages → fails on PFNMAP/IO (the I/O error we saw). No struct page.
- **Conclusion:** camera pixels are CPU-readable ONLY from a process that holds the mapping and
  loads it directly. The IMU "read shared memory" pattern DOES NOT extend to cameras.

### Corrected camera routes (route 1 is dead)
1. ~~Leech TS dmabufs read-only~~ — **DEAD** (VM_PFNMAP/IO + no fd steal on 4.4).
2. **LD_PRELOAD a frame-tap INTO trackingservice** — TS is `/system/bin/trackingservice`
   (SYSTEM linker namespace → `/data` preload works, PROVEN with `imu_shim`; and restarting TS
   does NOT trigger the HAL restart-cascade, so it's safe). From inside TS the dmabuf IS
   CPU-mapped → hook the frame-receive path (`libimagebuffer`/`FrameSet` consumer), copy pixels
   out to a socket/file. **Best bring-up route; no HAL fight, no fd/VM_PFNMAP problem.** Depends
   on TS actively receiving frames (streaming active).
3. **HIDL camera client** (`ICameraProvider`/`CameraStream`) — the HAL passes the dmabuf fd to
   the client legitimately over binder (hidl_handle fd-passing), so WE'd own the fd and can mmap
   it. But needs the HAL client-delivery gate cracked (same unproven gate as IMU).
4. Drive msm-config ourselves (replace the HAL's camera role) — heavy; needed only for a
   fully-blob-free ship path.

### Architecture note for the ship decision
- IMU: fully open (raw syncboss FIFO), no blob needed.
- Cameras: the HAL (sensors@1.0-service) is the hardware owner (exclusive msm-config). A
  pragmatic open stack = keep the HAL as the camera driver, replace trackingservice with
  Monado + a frame consumer. That consumer needs frames via route 2 (preload-tap, bring-up) or
  route 3 (HIDL client, needs gate). Fully-blob-free cameras = route 4.

## RETRY SESSION 5 (Claude) — CAMERA PRELOAD-TAP: MECHANISM PROVEN, INJECTION METHOD IS WRONG

Built `tools/cam_tap/img_shim.c` (plain C, no libc++; on device /data/local/tmp/img_shim.so):
hooks `OVR::Sensors::ImageBuffer::ImageBuffer(native_handle*,native_handle*)`
(`_ZN3OVR7Sensors11ImageBufferC1EPK13native_handleS4_`) + a bg scanner thread that mmaps each
pool dmabuf (DMA_BUF sync) and dumps any frame with real content.

**[VERIFIED-CLAUDE] The ImageBuffer ctor hands us everything per buffer:**
- arg b (graphic-buffer handle): `numFds=2` → **data[0] = pixel dmabuf (311296 B)**, data[1] =
  4 KB metadata; `numInts=24` ints = `{magic 0x676d736d "msmg", 0x40202a8, w=0x280=640,
  0x1e4=484, stride=0x280=640, h=0x1e1=481, fmt=0x10d, 0}`. arg a: 1 fd, 64 B (fence/meta).
- Fired **42× at TS startup then STOPPED** → it's the **pool-registration** hook (~11 buffers ×
  4 cams), NOT per-frame. Good for capturing the buffer set (fds+dims); not a frame trigger.
- **Reading pixels from inside TS WORKS** (the scanner's direct CPU byte-loads on the mmap'd
  dmabuf succeed — unlike external access_process_vm which fails on VM_PFNMAP). So a shim running
  *inside* TS can read frames. This is the key mechanism confirmation.

**[CORRECTED] Injecting via LD_PRELOAD requires restarting trackingservice, which disrupts the
session TEMPORARILY — but the preloaded instance DOES integrate and track.**
- `stop trackingservice` + manual `LD_PRELOAD` relaunch → display showed "3 dots" then a static
  image at first, BUT **user confirms it DID start tracking after a delay** → the static was just
  tracking-convergence timing, NOT a broken instance. The preloaded TS integrates with the
  runtime fine. (I rebooted prematurely on the wrong assumption it was broken.)
- Real cost: the restart briefly interrupts the session (~15–30 s reconverge) and CAN trigger the
  restart-cascade (SIGKILL'd the adb shell twice — issue `start`/`ctl.start` and don't rely on
  the shell surviving). Recovered cleanly (reboot → TS 892, HAL 768, Enforcing).
- **So the preload-tap IS viable:** relaunch TS with the preload, user wears headset, wait for
  tracking to converge (cameras stream), scanner captures + dumps a frame. Just be patient.
- Less-disruptive alt (no restart): ptrace/lldb `call dlopen(img_shim.so)` into the LIVE TS; the
  scanner then finds dmabufs by scanning `/proc/self/maps` for `anon_inode:dmabuf` 0x4c000 regions
  (ctor interposition won't fire on a late dlopen).

**[REVISED camera plan] Non-disruptive injection into the ALREADY-RUNNING TS:**
- Attach ptrace/lldb to the live TS (brief stop, tracking survives detach) and `call
  dlopen("/data/local/tmp/img_shim.so", RTLD_NOW)` in-process → loads the scanner into the LIVE
  session without restart. (dlopen won't interpose the ctor, so the scanner must discover the
  camera dmabufs another way: scan TS `/proc/self/maps` for `anon_inode:dmabuf` 0x4c000 regions
  and read those VAs directly — CPU loads work in-process.)  OR
- Build a **HIDL camera client** (ICameraProvider/CameraStream) — HAL passes the dmabuf fd over
  binder, we mmap it ourselves; clean, never touches TS — but needs the HAL client gate (unproven).
- Both still need cameras actually streaming (active tracking session = headset worn / VR app).

Device left clean after reboot; img_shim.so + dmabuf_read + mempeek + lldb-server remain on
/data/local/tmp for reuse.

## RETRY SESSION 5b (Claude) — CAMERA FRAMES CAPTURED CLEAN [VERIFIED-CLAUDE] ✅

**Cameras proven end-to-end.** Fixed the two problems from 5a:
1. Injection: relaunch TS with the preload, then WAIT — tracking/passthrough converges and cameras
   stream (user's correction was right; the static was just timing). Passthrough is the best
   trigger (Quest-1 passthrough = the 4 mono tracking cams streaming continuously).
2. Buffer discovery: the ImageBuffer-ctor-registered fds were the WRONG buffers (pool[5] rendered
   as banded garbage = a processed/non-frame buffer). The REAL frames are found by scanning
   `/proc/self/maps` (from inside TS) for `anon_inode:dmabuf` regions of ~311296 B and reading
   the mapped VA **directly** (in-process CPU loads work on VM_PFNMAP; external readers can't).
   → `tools/cam_tap/img_shim.c` v2 (constructor starts a scanner thread; dumps every frame-sized
   dmabuf with image content to /data/local/tmp/frame_<addr>.gray).

**Result:** 128 frame-sized dmabufs mapped in TS during passthrough; captured clean **640×480
8-bit grayscale** frames, stride 640, linear (vdiff 3.8–7.9 = smooth real images). Two exposure
groups (mean ~46 and ~71) = different cameras. Saved: `exports/camera-2026-08-31/camera_1.png`,
`camera_2.png` (+ .gray). Image content: real fisheye room view (monitor, keyboard, desk) — a
genuine OV7251 tracking-camera frame.

**Full sensor set now open:**
- IMU: raw syncboss FIFO (type 0x50), 1 kHz, accel(g)/gyro(deg/s)/temp — no HAL needed.
- Cameras: 640×480 mono fisheye, tapped from TS dmabufs via in-process /proc/self/maps scan.
  (Bring-up route; leeches TS. Ship-path for a blob-free stack would drive msm-config directly.)

**Caveat / restart hazard (again):** each TS relaunch drops video ~30 s and can wedge the
session; recovering cleanly needed a **reboot** twice. Device left clean (post-reboot: TS 895,
HAL 773, Enforcing). For future camera work, prefer the non-disruptive ptrace/lldb `dlopen`
injection into the LIVE TS instead of restart-with-LD_PRELOAD.

### Still open for cameras
- Per-frame timestamp + which dmabuf = which of the 4 cams (correlate with FrameSet FMQ).
- Frame↔IMU time sync (MontereyHostTimeSource / VsyncClock).
- Non-disruptive injection (dlopen into live TS) to avoid the restart/video-drop.

## RETRY SESSION 6 (Claude) — VIO TIME-SYNC SOLVED (hardware) [VERIFIED-CLAUDE] ✅

**Camera exposure timestamps are on the syncboss stream, on the SAME nRF 1 MHz clock as the IMU.**
Read `/dev/syncboss_stream0` during passthrough (non-disruptive, no restart) and found a THIRD
packet type beyond IMU(0x50)/broadcast(0xe0):
- **type 0x51, len 22, ~29.5 Hz** (ts deltas 33,890–33,896 µs = camera frame rate). One stream for
  all 4 hardware-synced cams (single exposure trigger).
- payload: `[0:4] u32 ts_us (nRF 1 MHz)`, `[4:8]=0`, `[8:20] 3× f32 (~5e-5, tiny — gyro-at-frame
  or exposure?)`, `[20:22] u16 (~0x1890, wanders — TBD)`.
- **Verified same clock:** each 0x51 ts falls exactly between its neighboring 0x50 IMU ts
  (e.g. IMU 1831827632 | CAM 1831828503 | IMU 1831828638). So camera↔IMU time-sync is FREE — no
  td estimation needed; both come from the same open kernel FIFO with comparable µs timestamps.

**VIO data status:**
- IMU: type 0x50, 1 kHz, {ts_us, accel(g), gyro(deg/s), temp} — syncboss FIFO. ✓
- Camera exposure ts: type 0x51, 30 Hz, ts_us same clock — syncboss FIFO. ✓
- Camera pixels: 640×480 mono ×4, trackingservice dmabuf tap (img_shim.c v2). ✓
- Calibration: factory Fisheye62 → KB4 (tools/quest_calib_convert.py, exports/calibration-*). ✓ (verify)

**Remaining for a Basalt dataset:**
1. Associate pixel-frames ↔ 0x51 timestamps (ordinal match: Nth 0x51 ts ↔ Nth quad-frame; handle
   drops). The pixel tap and the FIFO are separate streams captured together.
2. Which dmabuf = which of 4 cams (exposure/mean groups; or FrameSet camId).
3. Decode 0x51 floats + u16 (exposure? frame-id for robust association?).
4. Emit EuRoC-format dataset (cam0-3 + timestamps.txt, imu0.csv, calib) → run Basalt.

## RETRY SESSION 7 (Claude) — FrameSet DECODED [VERIFIED-CLAUDE]
Hooked `MessageQueue<FrameSet>::read` in trackingservice (fires during SUSTAINED tracking,
jiffies>40) and dumped the full 984-byte HIDL FrameSet as u64 words. Layout:
- 4 image sub-blocks, 12 words (96 B) each, at w2-13/14-25/26-37/38-49; then a trailer.
- **w11 = exposure timestamp in ns (CLOCK_MONOTONIC domain), SAME value across all 4 images**
  (hardware-synced). Verified: delta between framesets (140.5M ns) matches the ~136 ms frame
  spacing. This is the PRECISE per-exposure timestamp — and it's the same clock as the pixel
  tap's `mono_ns`, so frames can be assigned exact exposure times with no cross-clock offset.
- w13 (per block) = per-image timestamp; w2 lo32 = image index 0-3 = **camId**; w3 = frameset
  type (2/4); w9/w10 = doubles (exposure/gain). NO buffer pointers in the struct — buffers are
  referenced by index into the pre-shared pool (FMQ can't pass fds).
- FrameSet reads are IRREGULAR/batched (host deltas 13-40 ms), not clean 30 Hz.

### Remaining to a converging dataset
Link w11 (precise exposure ts + camId) to the pixel dmabufs. Cleanest: hook the OUTER
`DualStreamHandle<FrameSet>::read()` (returns OVR::Sensors::FrameSet with RESOLVED ImageBuffers →
pixel access) so ts+camId+pixels come together; OR a combined capture (FrameSet + pixel tap, both
mono_ns) and match by host time (now single-clock, tighter than the earlier nRF-vs-host matching).
This precise timestamp is exactly what the stereo Basalt run needed (the ~10 ms poll jitter was
the blocker).

## RETRY SESSION 8 (Claude) — VIO TRACKS ✅ [VERIFIED-CLAUDE]
Combined tap v6 (tools/cam_tap/img_shim.c): pixel scanner + FrameSet w11 hook + syncboss reader,
all CLOCK_MONOTONIC, GO-triggered. Captured pixels + w11 (mono exposure ts) + IMU/exposures (nrf).
- **Clock map (RANSAC value-match of w11<->0x51): mono_ns = 982.343*nrf_us + 4.296e13, residual
  0.23 ms (max 0.36 ms).** Sub-ms link between the camera (mono) and IMU (nRF) clocks.
- Built precise stereo dataset (exports/vio-precise): cam ts = w11 (mono); IMU = 0x50 converted
  nrf->mono; common clock; deduped over-captured address-groups to one-frame-per-w11.
- **Basalt basalt_vio (stereo-inertial) NOW TRACKS: 41 non-zero poses, NO NaN, path 0.378 m over
  0.9 s (bbox 23x28x6 cm = real head motion).** vs earlier jittery data -> NaN / all-zeros.
- => The ~10 ms poll-tap timestamp jitter was the convergence blocker; precise w11 + clock map
  (sub-ms) fixes it. FULL PIPELINE VALIDATED end-to-end: open IMU + open cameras -> hardware sync
  -> precise timestamps -> Basalt VIO trajectory.

Limit: only 45 pairs / 0.9 s (short sustained-tracking window + unstable dmabuf address-grouping
overlap). Longer sustained motion + a stable camera-id source (outer DualStreamHandle::read hook
giving atomic ts+camId+pixels) -> a full-length trajectory. Tooling (dataset builder, clock map,
Basalt docker) all in place to consume a bigger capture.

## RETRY SESSION 9 (Claude) — ATOMIC HOOK progress [VERIFIED-CLAUDE]
Built `tools/cam_tap/fs_atomic.c`: asm trampoline that interposes the sret-returning
`DualStreamHandle<FrameSet>::read()` (mangled `_ZN3OVR7Sensors11HidlWrapper16DualStreamHandle...4readEv`),
calls the real via GOT (`adrp :got:g_real`), dumps state, returns cleanly. **Trampoline works,
never crashes TS** (validated over multiple tracking sessions; read() fires ~12x per active burst).
- **read() returns a STATUS in x0/x1 = {1, 10}**, NOT the FrameSet (frame data is in the
  DualStreamHandle `this` object, not the return). read() only fires during ACTIVE tracking
  (sustained head motion).
- **Frame metadata found at `this[17],this[18],this[19]`** (3 mapped ~4KB regions ~0x1000-0x2000
  apart) containing `{0,0, ctrA, ctrB, 640,480,640,640(stride), 1, 0..., <tail32>}` where
  (ctrA,ctrB)=(6,4)/(5,2) per frame (frame#/camId?), tail32 changes per frame (ts/checksum?).
  These are per-frame descriptors, NOT the pixel buffers (too small; pixels referenced elsewhere).
- ImageBuffer is NON-polymorphic (no vtable) → can't ID by vtable; ID frame objects by 640x480.
- `this[+0xc0]` = config/strings (not frames). `this[30..39]` = another pointer array (0xe0 stride).

### Remaining (needs more capture iterations)
1. Trace the metadata descriptor -> pixel dmabuf (find the buffer fd/ptr/handle it references).
2. Pin exact timestamp field (correlate tail32 or a nearby qword with syncboss nrf/w11).
3. Confirm camId field. Then extract {ts, camId, pixels} atomically per frame -> dense dataset.
The asm-trampoline approach is proven safe; this is decode work, ~2-3 more short worn+moving captures.

STATUS: VIO validated (0.378m trajectory, exports/vio-precise/). Atomic hook ~60% done.

## RETRY SESSION 9b (Claude) — atomic hook: metadata YES, pixels NO (pool-referenced) [VERIFIED]
- Atomic read() hook (asm trampoline) cleanly yields the OVR::FrameSet metadata: per-frame
  exposure ts (CLOCK_MONOTONIC ns) + camId (image index 0-3), held at this[17..19] (3 mapped
  descriptor regions, 4 image blocks each). Same data as the inner MessageQueue<FrameSet>::read.
- **BUT pixels are NOT pointer-reachable from read()'s object graph.** Scanned this[0..90] and
  their members (2 levels) for a member ptr referencing 640x480 image content -> only false
  positives (small regions); the aggressive image-content scan (per-ptr sampled reads w/ SIGSEGV
  handling) CRASHED trackingservice (process-wide signal handling across threads). Pixels live in
  the pre-registered ImageBuffer pool, referenced by index, resolved elsewhere.
- => The atomic hook improves camId precision but does NOT give dense pixels. Dense pixel capture
  still needs EITHER (a) RE the ImageBuffer pool index->dmabuf resolution, OR (b) combine this
  precise camId+ts with the poll-tap pixels via timing. The poll-tap remains the (density-limited)
  pixel source. This is a real wall for the "one clean atomic capture" goal.
STATUS: VIO validated (0.378m). Atomic hook = clean metadata; pixel link unsolved.
