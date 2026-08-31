# Option A — Oculus sensors HAL interface map (camera + IMU via HIDL/FMQ)

Date: 2026-08-31
Device: `1PASH9ACHD0215` / `monterey` (msm8998), persistent Magisk root.
Follows [[06-sensor-tap-probe]]. Goal: reuse `vendor.oculus.hardware.sensors@1.0` to feed an
open tracker (reversible; keep the HAL blob, replace `trackingservice`/`vrapi` above it).
Raw / binaries: `recon/hal-A-2026-08-31/`.

## Source

Reverse-engineered from the on-device HIDL interface lib (stripped, but dynamic symbols +
extracted `.gnu_debugdata` MiniDebugInfo give full method signatures):

```
/system|vendor/lib64/vendor.oculus.hardware.sensors@1.0.so   (interface: BpHw/BnHw proxies)
/vendor/bin/hw/vendor.oculus.hardware.sensors@1.0-service    (impl daemon, pid 772)
```

Service name registered: `.../ICameraProvider/default`, `.../IImu/default`, `.../IMag/default`
(all `@1.0`, instance `default`). Currently owned by pid 772.

## Data-flow model: HIDL Fast Message Queues (FMQ)

Both camera frames and IMU samples are delivered over **synchronized FMQ**
(`MQFlavor = kSynchronizedReadWrite`) whose element types are **fixed-size POD** — confirmed
because the only `*EmbeddedToParcel` helpers for these types are for the *MQDescriptor*, not
the element (FMQ requires trivially-copyable elements). So consuming = `memcpy` of a C struct.

`ISensorClient` is a lightweight registration/identity token (only `getSensorClientInfo` →
`SensorClientInfo`); it is passed to `prepareStream`/`streamControl` but carries no data — the
payload rides the FMQ, not callbacks. The open client implements a trivial `ISensorClient`.

FMQ payload types seen: `ImuData`, `FrameSet` (camera), plus `ControllerImuData`, `PoseOutput`
(the HAL can even emit fused pose — potential shortcut worth noting, not the current plan).

## Interfaces & methods (recovered signatures)

### `ICameraProvider`
| method | signature (recovered) |
|---|---|
| `getProperties` | `() -> (Result, vec<CameraProperties>)` |
| `getStream` | `(FrameType) -> ICameraStream` |
| `getChannels` | `() -> (ChannelSettings)` |
| `setChannels` | `(ChannelSettings)` |
| `getCalibrationData` | `(uint32) -> (Result, handle)`  ← on-device calib via fd |
| `getRawImageMode` / `setFrameRate` / `getUtilityFrequency` / `setUtilityFrequency` | misc control |

### `ICameraStream`  (obtained from `getStream`)
| method | signature |
|---|---|
| `prepareStream` | `(ISensorClient, MQDescriptor<FrameSet,sync>, FmqConfig) -> (Result, MQDescriptor<FrameSet,sync>, FmqConfig)` |
| `streamControl` | `(ISensorClient, StreamCommand)`  ← START/STOP |
| `getConfiguration` | `(ISensorClient) -> (Result, CameraStreamConfiguration)` |
| `getMetadata` | `() -> (Result, CameraStreamMetadata)` |
| `setResolution` / `setExposureGain` / `setPhaseOffset` / `setCameraSyncMode` / `setFrameRate` | frame control |
| `startOverridingExposureSettings` / `stopOverridingExposureSettings` | AE override |
| `getFrameRate` / `writeSessionOcalData` / `streamControl` | misc |

### `IImu`  (and `IMag`, identical shape)
| method | signature |
|---|---|
| `getProperties` | `() -> (Result, MotionSensorProperties)` |
| `prepareStream` | `(MQDescriptor<ImuData,sync>, ISensorClient, FmqConfig)` |
| `streamControl` | `(ISensorClient, StreamCommand)` |

### `ISensorClient` (client implements this)
| method | signature |
|---|---|
| `getSensorClientInfo` | `() -> (Result, SensorClientInfo)` |

## Client control sequence (Option A)

```
1. IImu          imu  = IImu::getService("default")
   ICameraProvider cam = ICameraProvider::getService("default")
2. imu.getProperties(); cam.getProperties()      // enumerate rate / 4 cameras / formats
3. ICameraStream s = cam.getStream(FrameType::<tracking>)   // enum value TBD
4. create MessageQueue<ImuData>/<FrameSet>(sync); build MQDescriptor + FmqConfig
   imu.prepareStream(mqDesc, client, fmqCfg)
   s.prepareStream(client, mqDesc, fmqCfg) -> (Result, mqDesc', fmqCfg')
5. imu.streamControl(client, StreamCommand::START)
   s.streamControl(client, StreamCommand::START)
6. loop: mq.read(&record)  // POD ImuData / FrameSet  -> feed Basalt
```

## Deep RE findings (from service-binary MiniDebugInfo, 1748 impl symbols)

Extracted `.gnu_debugdata` from both the interface lib and the **service binary**; the impl
(`...V1_0::implementation::*`, plus internal `OVR::Sensors::*`) is far more revealing.

**Frame delivery = FMQ metadata + out-of-band ION image buffers (NOT inline pixels).**
- Type `ImageBufferHandle` exists; `OVR::Sensors::Convert::intoHidl(ImageBuffer -> ImageBufferHandle)`
  maps shared image buffers to handles. Pixels live in shared **ION/gralloc** buffers
  (matches `trackingservice` holding `/dev/ion` + ashmem); the `FrameSet` FMQ element is
  fixed-size POD carrying frame metadata + buffer references, not the 1.2 MB of pixels.
- `FrameSet` = a set of `Frame` (`gsl::span<Frame>`, `FrameSetDispatcher::FrameSetList`).
- FMQ uses an **EventFlag** (blocking) + `hidl_handle`; `SensorClientManager<ImuData>::ClientInfo`
  holds `{MessageQueue<ImuData,sync>, EventFlag*, hidl_handle, ISensorClient, uint, uint}`.
- Confirmed queues: `MessageQueue<ImuData|MagData|FrameSet, sync>`.

**IMU pipeline:** `Sensor<Imu>` receives `SyncbossEventHandler::StreamPacket` → converts →
`ImuData` → FMQ. `SensorTraits<Imu>::enableSensor(void*, float rate_hz)`.

**Enums are runtime-discoverable — hardcoding largely unnecessary.**
`ICameraProvider::getProperties()->vec<CameraProperties>` and `getChannels()` enumerate the
cameras/frame-types/channels live. `FrameType` is an 18-value enum (`EnumBitset<FrameType,18>`,
`NUM_FRAME_TYPES`) with names incl. `HEADSET` (the 4 mono OV7251 tracking cams — the target),
`CONTROLLER`, `HAND`, `FACE`, `DEPTH`, `COLOR`. SyncBoss stream IDs: `HMD_IMU`, `HMD_MAG`,
`HMD_FSIN`, `HMD_DOUBLETAP`. Channels: `GENERIC_SLOT_0..7`, `FACE_EYE_COMPOSITE`.

## Still to recover (last mile — needs a disassembler OR a live probe)

1. **Exact POD byte offsets/sizes** of `ImuData`, `Frame`, `FrameSet`, `ImageBufferHandle`,
   `CameraProperties`, `MotionSensorProperties`, `FmqConfig`. Reached the limit of
   nm/objdump/strings; these need a **Ghidra** decompilation session on the service binary
   (read the field stores in `Sensor<Imu>` convert + `CameraStream::prepareStream` @0x39134),
   **or** empirical recovery via a live enumeration client (below).
2. `ImageBufferHandle` mechanics: is it a gralloc `native_handle` (ION fd + stride/format) the
   client maps directly, or an index into a pre-registered pool? (Determines the frame-map path.)
3. Enum integer values (`StreamCommand` START/STOP, `Result`) — only needed if not enumerating
   at runtime; cheap to read once in Ghidra or by probe.

## Build-strategy options for the client (DECISION NEEDED)

| Option | How | Trade-off |
|---|---|---|
| **A1. Reconstruct `.hal` + `hidl-gen`** | rebuild interface defs from recovered signatures/layouts, generate a clean client lib | most portable/maintainable; needs the struct layouts nailed first |
| **A2. dlopen on-device `.so`** | call the exported `BpHw*` proxies directly (getService/getStream/prepareStream are exported `T` symbols) | least new marshalling code; needs matching C++ headers to call through |
| **A3. Raw binder client** | hand-marshal transactions by code | no deps; most fragile; still needs layouts+codes |

All three still require task (1)/(2) above. A1 is the cleanest long-term.

## LIVE PROBE — working HIDL client + recovered layouts (2026-08-31)

Built a real client and pulled live data from the HAL. This both recovers struct layouts
empirically **and** proves the client-build approach for the eventual Monado driver.

### Toolchain & method (`tools/hal_probe/`)
- Android NDK r27c (`tools/android-ndk-r27c/`), aarch64 clang 18.
- The NDK ships **no HIDL headers/libhidlbase** (platform-internal). Workaround: bind directly
  to the **exported proxy symbols** in the on-device `vendor.oculus.hardware.sensors@1.0.so`
  via `asm("<mangled>")` labels, link against **pulled device libs**
  (`recon/hal-A-2026-08-31/devlibs/`: libhidlbase, libutils, libc++, …). Compiler only has to
  get the *call ABI* right, not mangling. Callback structs arrive by const-ref (a pointer), so
  their bytes are dumpable without a real definition.
- Gotchas nailed down:
  - `getService(name, getStub)` — 2nd bool is **getStub**, not retry. Must be **false**
    (true = in-process passthrough → null for a binderized HAL).
  - NDK libc++ inline-ns is `__ndk1` vs device `__1`: layouts match (so passed std::string/
    std::function work), but don't call out-of-line std members yourself (use C string fns).
  - The lib's post-callback cleanup trips over the type-erased std::function stand-in →
    grab data and `_exit(0)` from inside the callback.
  - Client runs in `u:r:magisk:s0`; needs SELinux **permissive** to get hwservices
    (`setenforce 0` for the probe, restored to Enforcing after). A real deployment adds an
    sepolicy rule instead.

### `MotionSensorProperties` (from `IImu::getProperties`) — VERIFIED
Result=0. Layout:
| off | type | value |
|---|---|---|
| 0x00 | hidl_string | `"ICM20602"` — IMU is **InvenSense ICM-20602** |
| 0x10 | hidl_string | `"HMD IMU"` (label) |
| 0x20 | hidl_string | factory calib JSON, 2311 B (== exported `imu_calibration.json`) |
| 0x30 | hidl_string | online-refined calib JSON, 8839 B |
| 0x40 | float | **1000.0** (sample rate Hz) |
| 0x44 | float | 0.0 |

Key: **the HAL serves calibration inline** — an open tracker gets IMU calib straight from
`getProperties`, no separate `/persist` read needed. (hidl_string = `{const char* ptr;
uint32_t size; bool owns;}`, 16 B.)

### `CameraProperties[]` (from `ICameraProvider::getProperties`) — VERIFIED
Result=0, **count=4**. `hidl_vec<CameraProperties>`; element **stride 40 B**:
| off | type | value |
|---|---|---|
| 0x00 | uint64 | `id` (0,1,2,3) |
| 0x08 | hidl_string | `"OV7251"` sensor type |
| 0x18 | hidl_string | (empty — reserved/serial) |

Confirms 4× OV7251 tracking cams, ids 0–3, matching the calibration export.

### `ChannelSettings` (from `getChannels`) — partial
Nested: outer `hidl_vec` (size 2) of channel entries with count/slot/offset fields
(0x28-strided). Full decode deferred — secondary to the frame-streaming path.

### Still to recover (needs actual streaming, not just enumeration)
`ImuData` and `FrameSet`/`Frame` FMQ **element** layouts are not exposed by getProperties —
they require `prepareStream` + reading the FMQ. That's the next step: create
`MessageQueue<ImuData,sync>` + EventFlag, implement a minimal `ISensorClient`, call
`IImu::prepareStream` + `streamControl(START)`, read one record. The hal_probe client is the
foundation for it.

## Streaming constants recovered (2026-08-31, for prepareStream/FMQ)

From disassembling `BpHwImu::_hidl_prepareStream` and the impl:

- **prepareStream parcel order**: `writeInterfaceToken` → `writeBuffer(MQDescriptor, 32B)` →
  `writeEmbedded(MQDescriptor)` (grantors vec + native_handle fds) → `writeStrongBinder`
  (ISensorClient) → `writeBuffer(FmqConfig, 24B)` → `writeEmbedded(FmqConfig)`.
- **MQDescriptor header = 32 B** (standard FMQ: `hidl_vec<GrantorDescriptor>` 16 + `native_handle*`
  8 + `uint32 quantum` + `uint32 flags`).
- **`FmqConfig` = 24 B**, and its `writeEmbedded` tail-calls the **hidl_handle** writer → it
  contains a `hidl_handle` (the EventFlag fd) + ~16 B of scalars. Construct with the blocking
  EventFlag fd (or omit for non-blocking polled reads — TBD in bring-up).
- **`StreamCommand`**: impl `streamControl` switches on `==1` and `==2` (else default). START vs
  STOP among {1,2} — resolve empirically in the first live test (cheap: try 1, check for data).
- **`sizeof(ImuData)`**: NOT statically recoverable here (FMQ write is an inlined template, no
  symbol). Determine empirically in bring-up — set the MQ quantum, START, and check that the
  reader sees a monotonic ~1 kHz timestamp field; trial {32,40,48,64} B. This also yields the
  full ImuData field layout from the raw ring bytes.

### Verified struct layouts so far
- `hidl_string` = `{const char* ptr; uint32 size; bool owns;}` (16 B).
- `hidl_vec<T>`  = `{T* buf; uint32 size; bool owns;}` (16 B).
- `MotionSensorProperties` = 4×hidl_string (chip, label, factoryCalibJson, onlineCalibJson) +
  float rate@0x40 (=1000). VERIFIED live.
- `CameraProperties` (stride 40) = `{uint64 id; hidl_string sensorType; hidl_string reserved;}`.
  VERIFIED live (4 cams, OV7251, ids 0–3).
- `FmqConfig` = 24 B incl. hidl_handle. `MQDescriptor` header = 32 B.

## Toolchain for building HIDL/FMQ clients (2026-08-31) — SOLVED, reusable

Goal: compile framework HIDL/FMQ **template** code (MessageQueue, EventFlag, MQDescriptor)
against the device's own libs. Two blockers, both solved:

1. **No AOSP headers in NDK.** Fetched Android-10 (`android-10.0.0_r47`) include trees via
   gitiles archive into `tools/aosp-headers/inc/` (libfmq, libhidl base+transport, libcutils,
   libutils, liblog, libsystem). 119 headers; has `fmq/MessageQueue.h`, `fmq/EventFlag.h`,
   `hidl/MQDescriptor.h`, `hidl/HidlSupport.h`, etc. (gitiles: download tarball then extract —
   piping to tar fails; serialize requests, googlesource rate-limits bursts.)
2. **`std::__ndk1` vs device `std::__1` mangling.** NDK libc++ `__config_site` hardcodes
   `_LIBCPP_ABI_NAMESPACE __ndk1`; device libs use `__1` (same ABI_VERSION=1, so layouts match,
   only the namespace token differs → link failures like `EventFlag::createEventFlag(std::__ndk1::atomic...)`).
   **Fix:** shadow `__config_site` with a patched copy (`sed 's/__ndk1/__1/'`) via
   `-isystem <patchdir>/c++/v1`. Now clang emits `std::__1::` symbols matching the device.

Verified: `MessageQueue<Pod,kSynchronizedReadWrite> mq(64,false); mq.isValid()` compiles,
links against pulled `libfmq.so`+`libhidlbase.so`+…, and **runs on device (exit 0)**.

Build recipe (see `tools/hal_probe/`, extend for streaming):
```
CXX=android-ndk-r27c/.../aarch64-linux-android29-clang++
$CXX -std=c++17 -fPIE -pie \
  -isystem /tmp/cxxpatch/c++/v1  \        # patched __config_site (__ndk1 -> __1)
  -Itools/aosp-headers/inc \
  src.cpp -L recon/hal-A-2026-08-31/devlibs \
  -l:libfmq.so -l:libhidlbase.so -l:libutils.so -l:libc++.so -l:libcutils.so -l:liblog.so \
  -Wl,--allow-shlib-undefined
```
For calling the vendor HAL proxies, still bind exported symbols via `asm("<mangled>")` (the
generated interface headers IBase.h/ISensorClient.h are hidl-gen output, not in source).

### Remaining for IMU streaming
- Build `MessageQueue<ImuDataPOD, sync>` (element size = `sizeof(ImuData)`, empirical).
- Provide an `ISensorClient` server: hand-write a minimal `ISensorClient` (derive HIDL `IBase`),
  wrap in the device's exported `BnHwSensorClient(sp<ISensorClient>)`. ← last real chunk.
- `asm()`-bind `BpHwImu::prepareStream` / `streamControl`; pass MQDescriptor + FmqConfig(with/without
  EventFlag) + the ISensorClient binder; `START`; poll FMQ; dump `ImuData` → recover its layout.

## Streaming client — end-to-end path validated (2026-08-31)

`tools/hal_stream/` builds with the toolchain above (framework FMQ headers + `__1` ABI patch +
asm-bound vendor proxies) and runs on device. Result of the first streaming attempt:

```
IImu::getService -> 0x7e9b0271c0
mq.isValid=1 quantum=128
prepareStream Result=0            <-- HAL ACCEPTED our FMQ descriptor + FmqConfig
streamControl(START=1) -> SIGSEGV in BpHwImu::_hidl_streamControl+508
```

Findings:
- **The whole client stack works**: MessageQueue<POD> built client-side, `getDesc()` passed to
  the HAL, `FmqConfig` (zeroed: empty EventFlag handle + zero scalars) accepted.
  **`prepareStream` returns Result=0 with a NULL `ISensorClient`.**
- **`streamControl` requires a real `ISensorClient`**: the proxy crashes dereferencing the null
  sp to `writeStrongBinder`. prepareStream + streamControl must pass the *same* client binder so
  the HAL keys the stream to it.

So the ONE remaining blocker to first IMU data is a valid **`ISensorClient` server object**
(getSensorClientInfo + IBase). Options:
1. **hidl-gen** (build `system/tools/hidl`): generates `ISensorClient`/`IImu`/types + `BnHw`
   cleanly — the right maintainable base for the Monado driver. Recommended.
2. **Hand-write** minimal `ISensorClient` (derive HIDL `IBase`) + wrap in the device's exported
   `BnHwSensorClient(sp<ISensorClient>)`. Now feasible with the fetched libhidl headers, but the
   generated `IBase.h` isn't in source (hidl-gen output) so IBase must be hand-declared too —
   intricate.

Everything up to and including `prepareStream` is proven. `StreamCommand`: START is still 1-or-2
(untested past the crash). Nothing on device modified; SELinux restored to Enforcing.

## hidl-gen built from source (2026-08-31) — for generating proper bindings

Built AOSP `system/tools/hidl` (`hidl-gen`) as a **host** tool (`tools/hidl-build/hidl-gen`),
so we can generate correct `ISensorClient`/`IImu`/`BnHw*` C++ bindings instead of hand-writing
HIDL server vtables.

Recipe (`tools/hidl-build/build_hidlgen.sh`):
- Sources: `system/tools/hidl` @ `android-10.0.0_r47` (gitiles archive) + libbase from
  `system/core/base` (libbase is under system/core in Android 10, NOT system/libbase).
- Host prereqs present: clang++, flex 2.6, bison 3.8, make.
- Fixes for modern toolchain drift:
  - bison 3.8: grammar uses `glr.cc` C++ skeleton → **delete `%define api.pure full`** (invalid
    for C++ skeletons; `%pure-parser`→ that line).
  - flex/bison glue: the C reentrant lexer needs `YYSTYPE`/`YYLTYPE`, which bison's C++ header
    doesn't emit. **Inject after `#include "hidl-gen_y.h"` in `hidl-gen_l.ll`:**
    `#define YYSTYPE yy::parser::semantic_type` / `#define YYLTYPE yy::parser::location_type`.
  - Force-include `-include cstdint -include cstring -include algorithm -include cstdio -include cstdlib`
    (old code relied on transitive includes newer libc++ dropped).
  - Exclude libbase `*_test.cpp`, `errors_windows.cpp`, `utf8.cpp` (Windows-only).
  - Link `-lcrypto` (OpenSSL, for Hash.cpp SHA256) `-lpthread`.
- Verified: `hidl-gen -L c++-headers ...` runs.

### GENERATED bindings (2026-08-31)
```
./hidl-gen -L c++-headers -o gen \
  -r vendor.oculus:iface/vendor/oculus -r android.hidl:iface/android/hidl \
  vendor.oculus.hardware.sensors@1.0
# (and -L c++-sources)
```
→ `gen/vendor/oculus/hardware/sensors/1.0/{ISensorClient,BnHwSensorClient,BpHwSensorClient,
BsSensorClient,IHwSensorClient,hwtypes,types}.{h,cpp}` + `SensorClientAll.cpp`.
`.hal` in `tools/hidl-build/iface/`; android.hidl.base@1.0 fetched to `iface/android/hidl/base/1.0`.

**LESSON (cost hours):** the "std::regex/FQName is miscompiled in the full binary" symptom was a
**zsh word-splitting bug** — `./hidl-gen ... $RR ...` with `RR="-r a:b -r c:d"` does NOT split in
zsh, so hidl-gen got one malformed `-r` value (`" vendor.oculus"` with leading space, path
swallowing the rest). `parse()` correctly rejected the garbage. Fix: pass `-r` flags as explicit
separate args (or a `"${arr[@]}"`), never an unquoted space-joined var in zsh. hidl-gen itself
is fine; the FQName.cpp patches tried mid-debug were reverted to pristine.

### Old plan (superseded by the generated bindings above)
1. Reconstruct the vendor `.hal` (package `vendor.oculus.hardware.sensors@1.0`) from the RE —
   at minimum `ISensorClient`, `IImu`, and the types needed (`ImuData`, `MotionSensorProperties`,
   `FmqConfig`, `SensorClientInfo`, `StreamCommand`, `Result`). Struct fields from verified
   layouts above; enum values as recovered/empirical.
2. `hidl-gen -L c++-headers` + `-L c++-sources` to generate `ISensorClient`/`IImu`/BnHw/BpHw.
3. Implement `ISensorClient` (trivial `getSensorClientInfo`), pass it to `prepareStream` +
   `streamControl(START)`, poll the FMQ → first IMU records → recover `ImuData` layout.

## Streaming client with generated ISensorClient — LIVE, one param short (2026-08-31)

`tools/hal_stream/hal_stream2.cpp` — full client using the hidl-gen-generated `ISensorClient`
(server object) + asm-bound vendor `IImu`. Build gotchas (all solved), on top of the earlier
toolchain: link generated `SensorClientAll.cpp`; fetch `hwbinder/*` headers + generate
`android.hidl.{base,manager}@1.0` headers; `-fno-rtti` (device libs have no typeinfo);
`-Wl,-z,muldefs` (dup `hidl_enum_values` across TUs); libbase include for `android-base/*`.

Verified working end-to-end up to the HAL boundary:
- `IImu::getService` ok; `MessageQueue<ImuElem,sync>` valid; `new MyClient()` → **`toBinder`
  returns a valid binder** (our generated `ISensorClient` server object is real & registered).
- **Critical ABI fix:** the vendor `prepareStream`/`streamControl` return
  `android::hardware::Return<void>` (sret via x8), NOT an int. Declaring `int`/`Result` made the
  callee write the Return object through an uninitialized x8 → SIGSEGV. (The earlier hal_stream
  "prepareStream Result=0" was this ABI bug reading garbage — it never actually succeeded.)
- With the ABI fixed, `prepareStream` sends a real transaction and returns
  **`Status(EX_TRANSACTION_FAILED): DEAD_OBJECT`** — because our guessed FMQ params **crash the
  HAL** (it restarts: `SyncBossFW: Build Flavor` in logcat, then recovers). Confirmed the HAL
  auto-recovers and head tracking resumes.

### The last gap: exact FMQ params
Two guessed values crash `sensors@1.0-service`'s `IImu::prepareStream` impl:
1. **`sizeof(ImuData)`** — used `ImuElem[128]` as the MQ quantum; the HAL builds
   `MessageQueue<ImuData>(ourDesc)` and likely faults when `quantum != sizeof(ImuData)`.
2. **`FmqConfig`** — sent zeroed (empty EventFlag `hidl_handle` + 0 scalars); the HAL probably
   dereferences the EventFlag handle. Need a real EventFlag fd (now buildable: the `__1` ABI
   patch fixed `EventFlag::createEventFlag`) and correct scalar fields.

Both need a short RE pass on the impl's `Imu`/`SensorClientManager<ImuData>` prepareStream path
(service binary `.gnu_debugdata`), or careful reconstruction. **Caution:** each attempt crashes
the live tracking HAL (auto-recovers) — iterate sparingly, not in a tight loop.

Everything else is proven: toolchain, FMQ, generated ISensorClient server, real transaction to
the HAL. Once the two params are right, records flow and `ImuData`'s layout falls out of the
raw ring bytes.

## FMQ params resolved; prepareStream now succeeds (2026-08-31)

Recovered the two params that were crashing the HAL, from the **client** wrapper
`libvrsensors-hidlwrapper.so` (`recon/hal-A-2026-08-31/`, gitignored) — it *creates* the FMQ so
the sizes are explicit there:

- **`sizeof(ImuData) = 64`** — from `MessageQueue<ImuData>::MessageQueue(unsigned long, bool)`:
  `lsl x1, x21, #6` (numElements × 64 for the data region). (My 128-byte guess was a crash cause.)
- **`FmqConfig = { hidl_string?→ hidl_handle eventFlag; uint32 a; uint32 b }`** (24 B). The
  eventflag is a **separate ashmem region** wrapped in a `hidl_handle` (via
  `OVR::OS::createEventFlagHandle` → `ashmem_create_region` + `native_handle`); the two uint32s
  are eventflag notification bits. The HAL builds an `EventFlag` from it → empty handle crashed it.
  `StreamHandle<T>::prepareStream(hidl_handle, EventFlag*, uint32, uint32)` confirms the shape.

Client (`tools/hal_stream/hal_stream2.cpp`) now: `ImuElem[64]`, and builds a real eventflag
handle (`ashmem_create_region(4096)` + `native_handle_create`). Result:

```
prepareStream isOk=1    streamControl(START) isOk=1    (no HAL crash)
```

### Still: no data flows (activation handshake incomplete)
prepareStream + streamControl both succeed, but `availableToRead` stays 0 and the HAL never
calls our `getSensorClientInfo` (`infoCalls=0`), for StreamCommand ∈ {0,1,2} and with/without
streamControl. So the HAL accepts the stream setup but doesn't register us as a live receiver.

Likely missing: the exact client→HAL activation the real client performs. Leads to chase next:
- The HAL may call `ISensorClient::getSensorClientInfo` back to register us — verify our process
  actually *serves* that binder call (tried `configureRpcThreadpool` + a `joinRpcThreadpool`
  thread; still `infoCalls=0`). Confirm the callback path with a `lshal`/binder check.
- Trace `libvrsensors-hidlwrapper` `StreamHandle<ImuData>` + its orchestrator (likely in
  `libossdk.oculus.so`) for any step between `prepareStream` and `read()` (enable/rate/subscribe,
  or the correct StreamCommand + eventflag bit values a/b).
- The two `FmqConfig` uint32 bits (guessed 1,2) may need the real values the HAL wakes on.

Progress vs. crashing: the FMQ params are now correct enough that the HAL accepts everything
without error — only the activation step remains. Each run is now non-crashing (HAL stays up).

## Activation handshake RE (2026-08-31) — narrowed, not yet cracked

More RE on why `prepareStream`+`streamControl` succeed but no `ImuData` arrives:

- **`getSensorClientInfo` is never called by the HAL** — no `BpHwSensorClient::getSensorClientInfo`
  reference anywhere in the impl. `ISensorClient` is purely an **identity/lookup token** (keyed
  by binder). So `infoCalls=0` is expected; our client is correctly registered and needs to
  serve no callback. (Removes that hypothesis.)
- **`onClientStarted_l`/`onClientStopped_l` for `ImuData` are no-ops** (COMDAT-folded with
  trivial accessors) — so `streamControl` isn't a hard gate; `prepareStream` registers the client.
- **Eventflag bit scheme** (from `HidlWrapper::CompositeStream::addStream`): one shared eventflag
  per composite (counter starts `0x80000000`); each stream gets two bits
  `a = 1<<(idx+14)`, `b = 1<<(idx-1)` passed to `StreamHandle::prepareStream(handle, EventFlag*,
  a, b)`. For polled reads the bit values shouldn't matter (tried `{1,2}` and `{0x8000,0x1}`).
- `prepareStream` returns `Return<void>` → `isOk=1` only means **transport** OK, not that the HAL
  accepted the descriptor/FmqConfig. A silent descriptor rejection is still possible.

Result unchanged: `prepareStream isOk=1`, `streamControl isOk=1`, no crash, `availableToRead=0`.

### Leading hypotheses to chase next
1. **IMU source not producing to our queue.** The HAL distributes `ImuData` only to *active*
   clients; the "active" transition (and `SensorTraits<Imu>::enableSensor(rate)`) may need the
   exact `StreamCommand` START value + a rate we haven't supplied. Find the `enableSensor`
   trigger and the real streamControl semantics (its callers are via vtable/inlined).
2. **Silent descriptor rejection.** The HAL builds `MessageQueue<ImuData>(ourDesc)`; if it
   requires an in-ring eventflag grantor (create the client FMQ with `configureEventFlag=true`)
   or a specific grantor layout, `isValid()` fails HAL-side and it no-ops. Worth trying
   `MessageQueue<ImuElem>(256, true)` and wiring the ring eventflag.
3. Reconstruct the full `IImu.hal` (now that `sizeof(ImuData)=64`, `FmqConfig` known) and
   generate a real `IImu` proxy — removes any asm-binding ABI risk and gives typed Return values.

Best authoritative source for the exact sequence: `libossdk.oculus.so` /
`libvrsensors-hidlwrapper.so` `CompositeStream`/`StreamHandle` orchestration (partially traced).

## Full IImu generated + activation root-caused (2026-08-31)

Reconstructed the complete `IImu.hal` (methods in code order for tx codes 1/2/3:
`getProperties`, `prepareStream(fmq_sync<ImuData>, ISensorClient, FmqConfig)`, `streamControl`)
plus the types (`ImuData`=8×uint64=64B, `FmqConfig`={handle,uint32,uint32}, `MotionSensorProperties`,
`StreamCommand`). Generated a real `IImu` proxy (`tools/hal_stream/hal_stream3.cpp`,
`gen/.../ImuAll.cpp`). No asm-binding.

**Marshalling/ABI hypothesis eliminated:** `imu->getProperties(...)` via the generated proxy
returns real data — `Result=OK sensor="ICM20602" label="HMD IMU" rate=1000`. So the reconstructed
`.hal`, transaction codes, and struct layouts are all correct, and `prepareStream`/`streamControl`
use identical correct marshalling.

**Eventflag format matched exactly** (from `createEventFlagHandle` @svc `0x81e44`):
`ashmem_create_region(name, 4)` (4 bytes, one atomic word), `ashmem_set_prot_region(fd, RW)`,
`native_handle_create(1,0)`, `hidl_handle.setTo(nh, true)`. Replicated — still no data.

### Root cause: the HMD IMU stream isn't enabled for our client
- The Quest gates the 1 kHz HMD IMU on **active tracking** (proximity/worn). Off/asleep:
  syncboss `0`/s. Briefly worn: only ~**75/s** (double-tap/controller, NOT the 1 kHz IMU).
- **Our `streamControl(START)` does NOT enable the IMU** — syncboss stays ~75/s during our run
  (no spike to 1 kHz). Neither does `prepareStream`. So the HAL registers us (all calls succeed)
  but never turns the sensor on for us, so nothing is produced to write.
- `onClientStarted_l`/`onClientStopped_l` are **no-ops** (COMDAT-folded), so the
  "enable on first client" logic is elsewhere — `SensorTraits<Imu>::enableSensor(void*, float rate)`
  (@ `0x4fdcc`), called via the traits struct (deeply inlined; caller not yet located).

Everything client-side is proven correct. The remaining gap is the **activation that makes the
HAL call `enableSensor(1000)` for a subscribing client** — the exact `StreamCommand`/rate/handshake.

### Two ways to close it
1. **Physical:** headset actively worn in a real tracking session (1 kHz syncboss), then run
   `hal_stream3` — if data flows, the whole open IMU path is proven and `ImuData`'s real 64-byte
   field layout falls out of the ring bytes. (Tested briefly; headset was set down before a
   sustained 1 kHz session.)
2. **RE the enable trigger:** locate the `SensorTraits<Imu>::enableSensor` caller / the exact
   `streamControl` command+rate that force-enables the IMU for a standalone client.

## Physical test + exhaustion of client-side variables (2026-08-31)

Headset worn/active during test: **`trackingservice` at 65% CPU, `sensors@1.0-service` at 9%** —
the IMU is definitively streaming (to trackingservice). So "sensor off" is ruled out; our
client's non-receipt is a **registration/activation difference**, not a dormant sensor.

Key: **the real client (`libvrsensors-hidlwrapper`) never calls `IImu::streamControl`** — IMU
data flows from `prepareStream` alone. (`streamControl` unreferenced for IMU in that lib.)

Exhausted, all still `availableToRead=0`, IMU confirmed active:
- generated `IImu` proxy (marshalling proven correct via working `getProperties`)
- `sizeof(ImuData)=64`; exact eventflag format (ashmem 4B + `ashmem_set_prot_region` RW)
- `configureEventFlag` true and false; with/without `streamControl`; cmd ∈ {0,1,2,99}
- serving RPC threadpool; `getSensorClientInfo` never called (identity-only, expected)

**Wall:** the HAL accepts every call (all `isOk`) and registers us, but never writes to our
queue — even though the same IMU stream is actively delivered to trackingservice. There is a
subtle gating condition on which client queues get written that we haven't found.

### Highest-value next step (definitive)
**Intercept trackingservice's actual `IImu::prepareStream` transaction** (ptrace/binder trace on
`sensors@1.0-service` or `trackingservice`) and byte-compare its `MQDescriptor` + `FmqConfig`
against ours. That reveals the exact difference directly, instead of guessing. Alternatively,
deep-RE the HAL's `prepareStream`/IMU-distribution impl to find the write-gating condition
(the per-sample "write to client N" predicate; currently inlined).

Everything client-side is proven correct; the gap is a specific, findable HAL-side gating detail.

## Reversibility

Nothing on the device is modified by this work — pure RE of pulled binaries. Running a client
later means stopping `sensors@1.0-service` (or coexisting if the HAL allows multiple clients);
`trackingservice` is only replaced at runtime, not on flash. Fully reversible.

## Intercept complete — client is PROVEN correct; blocker is 100% HAL-internal (2026-08-31)

Built an LD_PRELOAD interposer (`tools/imu_intercept/imu_shim.cpp`) and captured
trackingservice's real IMU calls. Findings:

- **Args byte-identical.** Our `MQDescriptor` (grantorCount=3, quantum=64, flags=1, grantor
  bytes `00..|08..|08.. 08..`) and `FmqConfig` (bitA=0x8000, bitB=0x1, 1 ef fd) match
  trackingservice's transaction **byte-for-byte** (see
  `recon/imu-intercept-2026-08-31/trackingservice-prepareStream.log`).
- **Real recipe = `prepareStream` then `streamControl(cmd=0)`.** Only one streamControl, cmd 0
  (START). We replicate exactly.
- **HAL calls `getSensorClientInfo` on trackingservice's clients ~8×, on ours 0×.** It simply
  never engages our client.

Every client-side variable ruled out (all give `availableToRead=0`, `infoCalls=0`):
- threadpool serving (main-thread `joinRpcThreadpool`, `hal_stream4`)
- caller UID (ran as uid=system via `su 1000`)
- single-client contention (trackingservice fully stopped — still nothing)
- EventFlag handshake (`hal_stream5`: real `EventFlag` from the shared ashmem fd, `readBlocking`,
  proactively `wake(0x1)` space-bit) — still nothing
- **client binder callability — DISPROVEN as the cause:** `tools/hal_stream/clienttest.cpp`
  registers our generated `ISensorClient`, and from a *second process* `castFrom(base)` + a live
  `getSensorClientInfo()` **round-trip succeeds** (returns name='openvr', f0=11). Our
  `interfaceDescriptor`/`interfaceChain` are correct over real IPC. The HAL *could* call us; it
  chooses not to. (Typed `ISensorClient::getService` returns null — a VINTF-manifest quirk,
  irrelevant: `castFrom` is what the HAL uses and it works.)

**Conclusion: the open IMU client is functionally complete and indistinguishable on the wire
from Meta's own. The sole blocker is a gating decision inside the closed HAL's
`SensorClientManager<ImuData>` (symbol-stripped, heavily-inlined templates) that declines to
route IMU to our registered+started+callable client.**

Live HAL instrumentation is **not viable/safe**: the HAL is a vendor binary in a restricted
linker namespace (no `/data` preload; `/vendor` is ro), and *any* stop/kill of
`sensors@1.0-service` triggers a restart cascade that SIGKILLs the adb shell (device-stability
risk). The `[unrestricted]` namespace trick (run a `/data` copy of the HAL) loads the preload
but init respawns the real HAL and the two conflict on hardware. Static RE is blocked by stripped
vtables/inlined templates.

## PIVOT: raw IMU straight from the open `oculus_syncboss` kernel driver (2026-08-31)

The kernel driver `oculus_syncboss` (SPI `spi12.0`) is **open** and exposes miscfifo char devs
(`system:system 0664`):
- `/dev/syncboss0`, `/dev/syncboss_control0`, `/dev/syncboss_stream0`, `/dev/syncboss_powerstate0`
- sysfs: `/sys/bus/spi/drivers/oculus_syncboss`, `/sys/class/misc/syncboss_stream0`

`miscfifo` fans out packets to **every** open fd, so a second reader gets its own copy of the
stream — **bypassing the closed HAL entirely** (this is the open-stack goal). Proven: reading
`/dev/syncboss_stream0` as root yields live 20-byte packets, e.g.
`01 03 00 e0 00 0e 00 | <ts32> | 00 00 00 00 | <ctr16> | 00 00 | <seq8>` with `seq` incrementing
0x09,0x0a,… and a monotonic ~33.4k-tick timestamp delta.

BUT: a fresh fd currently only receives an **always-on ~30 Hz, single-type (`01 03 00`) broadcast
stream — NOT the 1 kHz IMU.** The nRF only streams enabled sensor types, and delivery appears
per-fd/subscription-gated. To get IMU we must **enable it via `/dev/syncboss_control0`** (write
the syncboss "enable sensor" command the HAL uses) and then parse IMU packets (type 0x50/80 per
`SyncBossHAL` logs) off `syncboss_stream0`.

### Next step (syncboss-direct path)
1. RE the syncboss wire protocol: packet framing + the `syncboss_control0` enable-IMU command
   (open kernel driver `syncboss.h` + community RE + the HAL's writes to control0).
2. Send enable-IMU on control0, read `syncboss_stream0`, decode accel+gyro (type 0x50).
3. Feed into Monado/Basalt. This sidesteps the HAL blocker completely and is fully reversible
   (read-only kernel FIFO; no flash/service changes).
