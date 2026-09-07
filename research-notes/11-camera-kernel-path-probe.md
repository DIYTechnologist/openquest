# Camera acquisition without trackingservice — feasibility probe (2026-09-01)

Answers "which path gets us our own HAL + tracking service?" Follows
[[10-CHECKPOINT-camera-tap]], [[07-hal-A-camera-imu-interface]], [[08-vio-status]].
Read-only probe: no device state changed, nothing opened, no capture.

## Why the existing camera tap can't be the runtime

The tap (`ibfs_hook.so`) is an LD_PRELOAD **inside** Meta's `trackingservice`, and frames only
flow while that service is actively tracking. It therefore requires the thing we want to
replace. It is a dataset/validation tool — it validated Basalt + the calibration conversion —
but it is structurally a dead end for the open-stack endgame. The remaining timestamp thread
in [[10-CHECKPOINT-camera-tap]] sharpens a tool we won't ship.

IMU is already blob-free (open `oculus_syncboss` kernel FIFO). **Cameras are the only thing left.**

## VERDICT: the kernel-direct camera path is viable

### The whole msm camera pipeline is in-kernel and open (verified on device)
`recon/config`: `MSM_CAMERA`, `MSM_CAMERA_SENSOR_DRIVER`, `MSM_CSIPHY`, `MSM_CSID`,
`MSM_ISPIF`, `MSM_CPP`, `VIDEO_V4L2_SUBDEV_API` — all `=y`, no modules (`/proc/modules` empty).

Live v4l2 topology (`/sys/class/video4linux/*/name`):
| node | name |
|---|---|
| `video0` | **msm-config** ← the msm_cam_config ABI |
| `video3..6` | **msm-sensor** ×4 ← one per tracking camera |
| `v4l-subdev0..2` | msm_csiphy ×3 |
| `v4l-subdev3..6` | msm_csid ×4 |
| `v4l-subdev8` | msm_sensor_init |
| `v4l-subdev9..12` | cpp, vfe ×2, msm_ispif |
| `v4l-subdev13` | msm_buf_mngr |

All `crw-rw---- system:camera u:object_r:video_device:s0` (root/su bypasses DAC; SELinux
permissive needed as with prior probes, or an sepolicy rule for a real deployment).

### The 4 tracking cameras are stock-shaped QC sensor nodes
`/sys/bus/platform/drivers/oculus,camera` is bound to
`ca0c000.qcom,cci:oculus,camera@{0,1,2,3}`; stock `qcom,camera` has **zero** bound devices.

Kernel source is published: `facebookincubator/oculus-linux-kernel`, branch
`oculus-quest-kernel-master`, **Makefile version 4.4.205 — exact match** for the device
(`4.4.205-perf+`, built 2024-07-31). Cloned to `work/oculus-kernel/` (gitignored).

`arch/arm64/boot/dts/oculus/vs1-camera.dtsi` (vs1 = Quest 1 board, cf. the `oculus_vs1`
platform driver) `/delete-node/`s the stock `qcom,camera@0..3` and substitutes
`oculus,camera@0..3` with an **identical stock QC property set**: `qcom,csiphy-sd-index`,
`qcom,csid-sd-index`, `qcom,cci-master`, `cam_vio/vana/vdig` supplies (pm8998 s4/l22/l14),
`pwdn-gpios`/`qcom,gpio-reset` via tlmm, `qcom,clock-rates = <24000000 0>`, `mount-angle=90`.

### ~~The one GPL gap~~ — WRONG, see the correction below
> **Superseded.** The `oculus,camera` driver IS published: it is
> `drivers/staging/oculus/mcu/syncboss/syncboss_camera.c`, part of the syncboss MCU driver rather
> than a separate sensor driver, which is why the grep below (scoped to the camera subsystem)
> missed it. There is no GPL gap. Kept for the reasoning trail only.

`grep -r 'oculus,camera'` over the published tree hits **only the .dtsi**. The `.c` that binds
that compatible is **not published**; stock `msm_sensor_driver.c` matches `"qcom,camera"` only
(`.driver.name = "qcom,camera"`, `msm_sensor_driver_dt_match[]`).

But the node names prove Meta's driver plugs into the **published stock framework**:
`"msm-config"` is set at `drivers/media/platform/msm/camera_v2/msm.c:1375` and `"msm-sensor"`
at `.../camera_v2/camera/camera.c:932` — both stock. Combined with the verbatim-stock DT
properties, Meta's driver is a lightly-renamed stock sensor driver. **The userspace ABI it
exposes is the stock, published one** (`VIDIOC_MSM_SENSOR_CFG` / `struct sensorb_cfg_data`,
`include/media/msm_cam_sensor.h`), which we now have in full.

### NEW architectural fact: syncboss gates camera power
`dmesg`: repeated `oculus_syncboss spi12.0: Turning on cameras` / `Turning off cameras`, and
`.../spi12.0/control/num_cameras = 4`. The nRF MCU — not just SoC regulators — controls camera
power/strobe. This explains why frames only flow during active tracking, **and we already own
that channel** (`/dev/syncboss_control0`, open kernel driver, from the IMU work).

### Where the missing userspace lives
The HAL service (pid 773) holds exactly **`/dev/video0` (msm-config) + 2× `/dev/ion`** — and
*not* `video3..6` or the subdevs (those are opened transiently). Its own binary has no
`/dev/video`, `VIDIOC`, or `msm_camera` strings; only `"OV7251"` (the `CameraProperties`
label). The msm-config driving code + the OV7251 register init tables are in its mapped vendor
libs: **`libqcameradriver.so`**, `libqcamerahal.so`, `libqcameraoculushal.so`, `libcamerahal.so`
(all `/system/vendor/lib64`, alongside the already-RE'd `libimagebuffer.so`).

Note the QC "sensor driver" model puts register init arrays in **userspace**
(`CFG_WRITE_I2C_ARRAY`), so those tables are in `libqcameradriver.so` — recoverable by RE, and
independently cross-checkable against mainline Linux's `drivers/media/i2c/ov7251.c`
(not present in this 4.4 tree, but upstream has a full 640×480 mono init sequence).

## LIB PULL + RE (2026-09-01) — the userspace is a thin, plain-C v4l2 mini-driver

Pulled to `recon/cam-userspace-2026-09-01/` (gitignored; via `su` copy to `/data/local/tmp`,
staging removed after — `/system/vendor/lib64` is not readable by the `shell` user).
Sizes are tiny: libqcameradriver 62 KB, libqcamerahal 37 KB, libqcameraoculushal 22 KB,
libcamerahal 32 KB. This is **not** the QC mm-camera stack; Meta wrote a lean direct-to-kernel
driver, which its own log strings call the **"mini-driver"**.

### The stack
```
libqcameraoculushal.so    qcamera_open / qcamera_start_all_sensors / qcamera_dequeue / ...
        |                 (plain C ABI, 18 exports)
libqcameradriver.so       control_* : v4l2 ioctls on /dev/media*, CSIPHY/CSID/ISPIF/ISP subdevs
        |                 (plain C ABI, ~40 exports)
kernel                    stock published camera_v2  +  oculus,camera sensor driver (unpublished)
```

`libqcameraoculushal.so` exports exactly the frame-source API we need:
`qcamera_open`, `qcamera_num_sensors`, `qcamera_get_sensor`, `qcamera_get_sensor_dim`,
`qcamera_query_buffer_dimensions`, `qcamera_start_sensor` / `qcamera_start_all_sensors`,
`qcamera_enqueue`, `qcamera_dequeue`, `qcamera_dequeue_nonblocking`, `qcamera_get_fd`,
`qcamera_stop_*`, `qcamera_release_sensor`, `qcamera_close`, `qcamera_read_temperature`.

`libqcameradriver.so` exports `control_init`, `control_camera_open`, `control_add_stream`,
`control_config_stream`, `control_map_stream_buf`, `control_start_streaming_all_sensors`,
`control_qbuf`/`control_dqbuf`/`control_dqbuf_nonblocking`, `control_get_num_of_cameras`,
`control_get_capability`, `handle_new_session_request`, `initialize_sensors`, … plus data
symbols `all_cams`, `camera_context`.

### Exact kernel ABI it uses (all published in `work/oculus-kernel/`)
`VIDIOC_MSM_CSIPHY_IO_CFG`, `VIDIOC_MSM_CSID_IO_CFG`, `VIDIOC_MSM_ISPIF_CFG`,
`VIDIOC_MSM_ISPIF_CFG_EXT`, `VIDIOC_MSM_ISP_{INPUT_CFG, REQUEST_STREAM, CFG_STREAM,
UPDATE_STREAM, RELEASE_STREAM, REQUEST_BUF, ENQUEUE_BUF, RELEASE_BUF, SMMU_ATTACH,
AHB_CLK_CFG}`, plus stock `VIDIOC_S_FMT / S_PARM / REQBUFS / QBUF / DQBUF / STREAMON`.
Opens `/dev/media%d` and enumerates subdevs by name.

### KEY RESULT: the OV7251 register tables are NOT in userspace
`libqcameradriver.so`'s undefined imports are **libc only** (`ioctl`, `mmap`, `poll`,
`pthread_*`, `opendir`/`readdir`, `fopen`/`fscanf`) — no `dlopen`, no sensor lib, and there is
no `libmmcamera*`/ov7251 lib anywhere in `/system/vendor/lib64`. `.rodata` is ~16 KB and is
almost entirely the log format strings dumped above.

So sensor power-up and CCI/I2C init happen **in the kernel**, inside the unpublished
`oculus,camera` driver, reached via the `msm_sensor_init` subdev probe (`initialize_sensors`,
"Failed to initialize and probe sensor, status: %d"). **We never need the register tables** —
that removes the biggest RE cost from the plan. Userspace only configures the CSI/ISP pipeline
and pumps buffers, and every one of those ioctls is in the published tree.

## Plan for Route B — now split into two concrete variants

**B1 (fast, one thin blob left).** `dlopen` `libqcameraoculushal.so` from our own process and
call `qcamera_open` / `qcamera_start_all_sensors` / `qcamera_dequeue`. Plain C ABI, so no HIDL,
no `ISensorClient`, no hidl-gen — none of the machinery that stalled [[07-hal-A-camera-imu-interface]].
This alone **replaces `trackingservice` and the sensors HAL service**: our process owns the
cameras, syncboss already gives us the IMU, Basalt does the VIO. The remaining vendor blob is a
22 KB + 62 KB v4l2 shim, not a tracking algorithm.

**B2 (pure userspace).** Reimplement `libqcameradriver.so` against the published headers — we
now know the exact ioctl set and it's all in `work/oculus-kernel/`. Fully open userspace; the
only closed piece left is the in-kernel `oculus,camera` sensor probe (avoidable only by porting
mainline's `drivers/media/i2c/ov7251.c`, a separate project).

Recommended order: B1 first to prove frames flow outside trackingservice, then B2 to retire the
blob, with B1 as the reference implementation to diff against.

### Bring-up steps (first one that touches device state)
1. Build a minimal `dlopen`+`qcamera_*` client with the NDK (link against pulled devlibs, as in
   `tools/hal_probe/`). Note `libqcameraoculushal.so` needs `libqcameradriver.so`,
   `libcamera_metadata.so`, `libcutils`, `liblog`, `libc++`.
2. **Stop `trackingservice` AND `vendor.oculus.hardware.sensors@1.0-service`** — pid 773 holds
   `/dev/video0` and owns the pipeline; do not open the cameras concurrently with it.
   Restart-cascade hazard from [[10-CHECKPOINT-camera-tap]] applies, and note 07 records that
   killing the sensors HAL is itself cascade-prone. `setenforce 0` for the run.
3. `qcamera_open` → `qcamera_num_sensors` (expect 4) → `qcamera_query_buffer_dimensions`
   (expect 640×481 mono8, matching the tap) → `qcamera_start_all_sensors` → `qcamera_dequeue`.
4. If no exposures arrive, check `dmesg` for `oculus_syncboss: Turning on cameras` — camera
   power may need a syncboss control command via `/dev/syncboss_control0`.
5. Validate pixels against `exports/ib-capture-2026-09-01/` (known-good frames from the tap).

### Open questions for bring-up
- Does the syncboss camera-power gate trigger from the driver stack, or must we request it?
- Does `libqcameraoculushal.so` produce `ImageBuffer`s / gralloc handles, or plain mapped
  buffers? (`libimagebuffer.so` sits alongside it, and `qcamera_dequeue` + `/dev/ion` in
  `libqcameraoculushal.so` suggests ION-backed buffers we map ourselves.)
- Exposure timestamps: does the dequeued buffer carry the `w11`-equivalent hardware ts? If so
  the open thread from [[10-CHECKPOINT-camera-tap]] dissolves entirely — no stream correlation,
  because we own the pipeline.

Route A (be the HIDL camera client of Meta's HAL) is now a **distant fallback**, not a plan:
B1 reaches the same place with a plain C call and sidesteps the `SensorClientManager` gating
that refused a byte-identical IMU client.

## B1 BRING-UP — ABI recovered, stage 0 PASSES live (2026-09-01)

Tool: `tools/cam_direct/cam_direct.c` (plain C, dlopen; NDK `aarch64-linux-android29-clang`).
Staged deliberately, because `__android_log_assert()` **aborts** — the lib asserts on a bad
`qcamera_open` mode, an out-of-range sensor index, and `dequeue`/`get_fd` on a sensor that
isn't started.

### Signatures recovered by disassembly (llvm-objdump, callee + call sites in libqcamerahal.so)
| function | signature | confidence |
|---|---|---|
| `qcamera_open` | `void*(int mode)` — mode asserted ∈ {0,1}; libqcamerahal picks it from a property (value 8→0, 10→1) ⇒ **bit depth**; returns calloc'd 112 B ctx (num_sensors @+0x48, sensor slots from +0x50) | confirmed |
| `qcamera_num_sensors` | `int(void)` — no handle; wraps `control_get_num_of_cameras()`, masked to u8 | confirmed |
| `qcamera_get_sensor` | `void*(void* hal, int idx)` — asserts `0 <= idx < n` | confirmed |
| `qcamera_get_sensor_dim` | `void*(void* sensor)` — literally `add x0,x0,#0x10; ret` | confirmed |
| `qcamera_query_buffer_dimensions` | `int(int cam, int meta_row, u32* w, u32* h)` — `*h = cap.h + (meta_row&1)` | confirmed |
| `qcamera_get_fd` | `int(void* sensor)` — asserts started, then `control_get_fd(cam)` | confirmed |
| `qcamera_dequeue` | `void*(void* sensor, int arg)` — wraps `control_dqbuf` result in an 80 B container, returns `container+8`; 2nd arg meaning unknown | confirmed shape |
| `qcamera_enqueue` | `int(void* sensor, void* frame)` — re-derives `frame-8`, `control_qbuf`, poisons +0x48 with `0xa5a5a5a5`, frees | confirmed |
| `qcamera_start_sensor` | `int(sensor, const void* dim{u32 w,h}, const void* cfg /*28 B*/, int nbufs, void* p4, int meta_row)` | **partial — `cfg` layout and `p4` unknown** |

**Sensor object layout:** `+0x08` u32 camera index, `+0x0c` u32 started flag,
`+0x10` `{u32 w, u32 h}`, `+0x28..0x44` the 28-byte cfg copied in by `start_sensor`.

Two other useful facts from the strings/disasm: `qcamera_start_all_sensors` logs
"*Qcamera starting all sensors with I2C broadcast...*" — that's the mechanism behind the
hardware-synced exposures across all 4 cams — and buffers are ION (`/dev/ion` in the lib).

### Stage 0 (dlopen + dlsym only) — PASSED on device, nothing touched
```
[+] dlopen /system/vendor/lib64/libqcameraoculushal.so -> 0xe44d...
[+] all 14 qcamera_* symbols resolved
[+] stage 0 (probe) OK — no hardware touched
```
This answers the linker-namespace question: **a `/data` binary running as root can dlopen the
vendor camera stack** — no restricted-namespace problem like the one that blocked preloading
into the sensors HAL in [[07-hal-A-camera-imu-interface]]. Ran with the HAL and trackingservice
still up; head tracking undisturbed.

### LIVE RESULT (2026-09-01): we drive the cameras from our own process. One gap left.
Runner `tools/cam_direct/run_bringup2.sh` (detached + a watchdog that unconditionally restores).

**Achieved, with `trackingservice`, the framework, and the sensors HAL all stopped:**
- `qcamera_open(0)` OK; **`qcamera_num_sensors() = 4`**; all 4 sensors acquired.
- `qcamera_query_buffer_dimensions(8,1)` → **640×481**, `(8,0)` → **640×480** — matches the tap.
- `qcamera_query_sensor_info` → `{w=640, h=480, 0x70, bpp=8, 0, 8, 1, 1, …, camIdx@0x24}`.
- **`qcamera_start_sensor` rc=0, sensor started flag=1**, dims 640×481, `qcamera_get_fd` → 12.
- Vendor's own log: `QCameraOculusHAL: Camera 0: Started camera`, `Setting num_bufs as 4`,
  `QCameraDriver: Camera N: mini-driver opened camera` for all four.
- Kernel: **`msm_csid_init: CSID_VERSION = 0x30050000` for CSID0–3** — we initialised the CSI
  receivers ourselves.

**The one gap: no frames.** `control_dqbuf timed out while polling`; `qcamera_dequeue` returns
NULL in 0.02 ms. Cause is almost certainly the **syncboss camera gate**: the OV7251s are
hardware-synced slaves driven by the nRF's FSIN strobe (`HMD_FSIN` is one of the syncboss stream
IDs in [[07-hal-A-camera-imu-interface]]), and dmesg only ever logs
`oculus_syncboss: Turning on cameras` when Meta's stack runs. Pipeline is up; nothing is
triggering exposures. **Next step: enable the camera trigger over `/dev/syncboss_control0`**,
the channel we already own from the IMU work.

### Syncboss camera power — SOLVED; MCU frame trigger — still open (2026-09-01)
**Correction to an earlier claim in this note: the `oculus,camera` driver IS published.** It is
`drivers/staging/oculus/mcu/syncboss/syncboss_camera.c` (`.compatible = "oculus,camera"`,
`.name = "oculus,camera"`), i.e. part of the syncboss MCU driver, not a separate sensor driver —
which is why the earlier recursive grep for it under `drivers/media/` found nothing. There is no
GPL gap, and B2 therefore has full source for the sensor side too.

Camera power is gated by a **snooped write** to `/dev/syncboss0`: `queue_tx_packet()` reads the
first byte of any write and dispatches (`syncboss_spi.c`):
- `SYNCBOSS_CAMERA_PROBE_MESSAGE_TYPE   = 40` → "Turning on cameras" → `enable_cameras()`
  (regulators + MCLK for all 4 sensors; `CONFIG_SYNCBOSS_CAMERA_CONTROL=y` on this device)
- `SYNCBOSS_CAMERA_RELEASE_MESSAGE_TYPE = 41` → "Turning off cameras"

Packet is `struct syncboss_data { u8 type; u8 sequence_id; u8 data_len; u8 data[]; }` → write the
3 bytes `{40,0,0}`. **`stop_streaming_locked()` force-releases the cameras when the last streaming
client closes**, so `/dev/syncboss_stream0` must be held open for the whole session. Both are now
done inside `cam_direct` (stage 0b). Verified live: dmesg logs "Turning on cameras" on our write.
**No wearing, motion, or proximity is involved** — with Meta's stack stopped, none of that logic
runs; the gate is purely this MCU command.

**`cam_format` resolved: 112.** `mm_stream_get_v4l2_fmt`'s jump table (base `0xcfa8`, table
`0x4304`, indexed by `fmt-1`) maps only **101 and 112** to fourcc `'GREY'`. Of those, 112 is the
one `qcamera_start_sensor` accepts (rc=0); 101 returns -1. The earlier guess of 42 passed
`mm_stream_calc_offset_raw`'s 2..123 range check but had no V4L2 mapping →
`mm_stream_get_v4l2_fmt: Unknown fmt=42` → sensor configured with a bogus pixelformat.

**Still no frames.** With cam_format=112, cameras powered, and streaming client held:
`control_dqbuf timed out while polling` after ~3 s of retries. The kernel shows CSID0–3
initialised but **no SOF/data IRQs** — so the sensors are powered and clocked but not exposing.
`enable_cameras()` only brings up rails + MCLK; the OV7251s are FSIN slaves and the MCU is not
strobing them.

**Next step: find the MCU command that starts the FSIN strobe.** The kernel only interprets a
few message types (`GET_DATA=2`, `SET_DATA=3`, camera 40/41, shutdown 90, prox 203/204/205/207/212,
wakeup 244) and forwards everything else opaquely to the MCU. So the camera/FSIN stream enable is
almost certainly a **type-3 SET_DATA packet whose sensor/stream id is defined in Meta's userspace**
(`libvrsensors-*.so` / the sensors HAL service), not in the kernel. `HMD_FSIN` is one of the
syncboss stream IDs already recovered in [[07-hal-A-camera-imu-interface]]. RE that id, send the
packet, re-run stage 3.

### FSIN / MCU frame trigger — FOUND: `libsyncboss.so` (2026-09-01)
The earlier guess that the stream-enable ids lived somewhere opaque in the HAL was wrong in a
useful way: `SensorTraits<Imu>::enableSensor` (@svc `0x4fdcc`) is a 17-instruction wrapper that
just calls **`syncboss_imu_enable@plt`**. The whole MCU command set is a **plain-C library**,
`/system/vendor/lib64/libsyncboss.so` (229 KB, deps: liblog/libm/libdl/libc only — trivially
dlopen-able, same as the camera stack). Pulled to `recon/cam-userspace-2026-09-01/`.

Camera-relevant exports:
`syncboss_camera_init`, `syncboss_camera_probe`, `syncboss_camera_release`,
**`syncboss_camera_start_streaming`**, `syncboss_camera_stop_streaming`, `syncboss_camera_deinit`,
`syncboss_camera_set_bpp`, `syncboss_camera_set_frame_rate`, `syncboss_camera_set_exposure_gain`,
`syncboss_camera_set_ctrl_exp`, `syncboss_camera_set_frame_tag_mode`, `syncboss_camera_params`,
`syncboss_camera_serial_num`, `syncboss_camera_register_read`/`_write`,
`syncboss_camera_decode_metadata`, plus `syncboss_init`/`syncboss_deinit`,
`syncboss_imu_enable`/`_disable`.

**MCU message types decoded from the disassembly** — each builds exactly the kernel's
`struct syncboss_data {u8 type; u8 seq; u8 data_len; u8 data[]}` as a little-endian word:
| function | word built | packet bytes | note |
|---|---|---|---|
| `syncboss_camera_probe(h, u8* out)` | — | `{40,0,0}` | type `0x28`; response type `0x46`(70) read back into `*out`. This is the kernel-snooped "Turning on cameras". |
| `syncboss_camera_init(h, u8 a)` | `0x0001002e` | `{46,0,1,a}` | type `0x2e` |
| **`syncboss_camera_start_streaming(h, u8 a)`** | `0x0001002c` | **`{44,0,1,a}`** | type `0x2c` — **this is the missing frame trigger** |
| `syncboss_camera_set_bpp(h, u8 bpp)` | — | type `0x8c`(140), 1 data byte | request/response helper |

So the sequence we were missing is `camera_init` → `set_bpp` → `set_frame_rate` →
`camera_start_streaming`, not just `camera_probe`. Probe alone powers rails+MCLK (which is what
we achieved); type 44 is what makes the MCU actually strobe FSIN.

### *** B1 ACHIEVED: frames dequeued from our own process (2026-09-01) ***
`qcamera_dequeue` returns real buffers, with `trackingservice`, the Android framework and the
sensors HAL all stopped. Two final pieces, both from `MontereyCameraProvider` in the HAL:

1. **Ordering.** The MCU and v4l2 sides are separate providers, and the MCU start must come
   **after** the v4l2 pipeline is streaming: `MontereyCameraProvider::cameraProbe()` does
   probe/`set_bpp`/`camera_init` up front, but `startCameras()` (`set_frame_tag_mode` +
   `start_streaming`) runs last. Sending `start_streaming` before `qcamera_start_sensor` strobes
   into a receiver that isn't listening → no frames.
2. **`syncboss_camera_set_frame_rate` takes a PERIOD in microseconds, not an fps.** The HAL calls
   it from `MontereyCameraProvider::applyFramePeriod()` with a u32 payload (msg type 137,
   data_len 4). `30` → rc=-1; **`33333` (= 30 Hz) → rc=0**.

Working sequence (all from our own process):
```
syncboss_init(&h, NULL)                     rc=0
syncboss_camera_probe(h, &out)              rc=0, out=0x0f  (bitmask: 4 cameras present)
syncboss_camera_set_bpp(h, 8)               rc=0
syncboss_camera_init(h, 4)                  rc=0
  -> qcamera_open(0) / get_sensor / qcamera_start_sensor(cfg[0]=112)   rc=0
syncboss_camera_set_frame_rate(h, 33333)    rc=0     (30 Hz period, microseconds)
syncboss_camera_set_frame_tag_mode(h, 1)    rc=0
syncboss_camera_start_streaming(h, 4)       rc=0     <-- FSIN strobe starts
  -> qcamera_dequeue(sensor, 0)             returns a frame in ~0.1 ms
```

**Frame layout recovered from the live dumps.** `qcamera_dequeue` returns `container+8`:
| offset | meaning |
|---|---|
| container `+0x00` | `driver buffer*` (the `control_dqbuf` result) |
| container `+0x08` / `+0x10` | timestamp **seconds / nanoseconds** — `83998.346006000` matched kernel uptime, i.e. **CLOCK_MONOTONIC** |
| container `+0x18` | pointer (stream info) |
| container `+0x38` | **page-aligned pointer — the pixel buffer** |
| drvbuf `+0x08`/`+0x10` | same sec/ns timestamp |
| drvbuf `+0x2c` (u32) | **307840 = 640 × 481** — the frame size in bytes |
| drvbuf `+0x20` | plane count (1) |

So each frame arrives with its own monotonic timestamp already attached — **the stream-correlation
problem from [[10-CHECKPOINT-camera-tap]] disappears entirely**, because we own the pipeline.

### PIXELS VALIDATED — B1 complete (2026-09-01)
8 frames dumped from camera 0 and rendered: **a real fisheye view of the room** (cables, desk
surface, shelving), full dynamic range (min 0, max 255). Data + PNGs in
`exports/cam-direct-2026-09-01/`. Per-frame: 307840 B = 640×481 mono8, CLOCK_MONOTONIC timestamps
**16.7 ms apart** (note: that's 60 Hz for a 33333 µs / 30 Hz period — worth checking whether the
period is per-camera-pair or the MCU strobes at 2× — see open questions).

**This is the milestone: an open process owning the Quest 1 cameras end to end.** IMU was already
blob-free via the syncboss kernel FIFO; calibration is exported; Basalt is built. The only vendor
code left in the loop is `libqcameraoculushal.so` + `libqcameradriver.so` + `libsyncboss.so`
(~310 KB of v4l2/SPI shims, no tracking algorithms) — which is exactly what B2 retires, and we now
have full kernel source for both the camera pipeline and the `oculus,camera`/syncboss driver.

Device restored cleanly after every run (HAL + trackingservice running, framework up, Enforcing).

### All 4 cameras + exposure control — WORKING (2026-09-01)
`cam_direct all <cam> <rounds> <exposure> <gain>` starts every sensor individually (the
known-good `qcamera_start_sensor` path, one acquire per sensor) and then round-robins
`qcamera_dequeue` across them.

- **4/4 sensors stream simultaneously** (`start_sensor` rc=0, distinct fds 30/43/54/66).
- **Exposure timestamps are hardware-synchronised in two groups of two**: cam0 and cam1 carry a
  byte-identical timestamp, cam2 and cam3 carry theirs, with only ~80 µs between the groups
  (e.g. `87479.488672000` for 0/1 vs `87479.488755000` for 2/3). The FSIN strobe is doing exactly
  what stereo VIO needs. **They are not a same-scene stereo pair** — the four cameras point in
  different directions (cam0 sees the desk/monitor, cam1 sees the keyboard and a different wall);
  they are time-synchronised views with partial overlap, which is why
  `exports/vio-stereo-2026-09-01/` carries several pair calibrations (01, 02, 03, 13, 20, 23).
- **`syncboss_camera_set_exposure_gain(h, u16 exp[4], u16 gain[4], 4)` works** (msg type 42,
  16-byte payload, count asserted == 4). Measured cam0 frame means in a lit room:
  | setting | mean |
  |---|---|
  | MCU default (no call) | 14.9 |
  | exp=2000 gain=200 | 36.2 |
  | **exp=8000 gain=255** | **44.2 — best** |
  | exp=20000 gain=255 | 14.9 (wraps: exceeds the 33.3 ms frame period) |
  So exposure is in units that saturate near the frame period; keep it well under ~20000.
- Frames at exp=8000/gain=255 are **sharp, well-exposed fisheye images** (means 33–120 depending
  on camera and what it faces). Data + PNGs: `exports/cam-direct-2026-09-01/` (24 frames, 4 cams).

**Lesson:** an earlier all-dark capture (mean ~4.5) was diagnosed as an exposure-scale bug. It
wasn't — the room light was off. Check the physical scene before concluding an API is misbehaving.

### First VIO capture from the open stack (2026-09-01, headset worn + moving)
`tools/cam_direct/{run_capture.sh}` + `cam_direct capture 0 25 8000 255` — cam0+cam2 (the pair
behind `exports/vio-precise/trajectory_calib02.txt`) plus the syncboss IMU, all from one process.
Raw data: `exports/vio-direct-2026-09-01/` (907 MB, 3010 frames + `syncboss.raw`).

- **Frames**: 1505 per camera over 25.2 s. Sharp, well-exposed worn-headset views.
- **IMU**: type 0x50 @ **993.8 Hz** (25152 samples) — `syncboss_imu_enable(handle)` (msg type 110)
  is all that's needed, and our own process reads `/dev/syncboss_stream0` directly.
- **ALTERNATING EXPOSURE**: frames arrive at 59.6 Hz alternating bright (mean ~85) and very dark
  (mean ~4.5). This is the Quest interleaving long-exposure **SLAM** frames with short-exposure
  **controller IR-LED** frames (cf. `syncboss_camera_set_ctrl_exp` / `_set_controller_exposure_gain_tag`).
  For VIO, keep only the bright half → ~30 Hz. The 481st row is a metadata row and differs between
  the two types (`libsyncboss.so` even exports `syncboss_camera_decode_metadata` to parse it).
- **Clock bridge found**: syncboss packet type **`0xe0`, len 14** at **29.6 Hz** — layout
  `{u8 flags; u32 ts_us (nRF clock); u32 pad; u32 frame_counter; u8}`, counter increments once per
  strobe. (The older `0x51` in `tools/vio/sb_decode.py` does not appear on this firmware path.)
  Pair `0xe0` in order against the bright frames' CLOCK_MONOTONIC stamps to fit
  `mono_ns = a*nrf_us + b`, then convert the IMU to monotonic. Also seen: `0x46` len 1 ×1 = the
  camera-probe response.

### Open questions for next session
- Frame interval is ~16.7 ms, not the 33.3 ms implied by `set_frame_rate(33333)`. Units confirmed
  as a period (33333 accepted, 30 rejected) but the effective rate is 2x — worth confirming.
- **Next milestone: Basalt.** Frames and the syncboss IMU are now both on CLOCK_MONOTONIC, so no
  RANSAC clock map and no stream correlation are needed at all. Pick a camera pair with real FOV
  overlap (see the existing `calib_*.json` in `exports/vio-stereo-2026-09-01/`), write a EuRoC
  dataset straight out of `cam_direct`, and produce a long trajectory — the goal from
  [[08-vio-status]].
- Then B2: reimplement the ~310 KB of vendor shims against the published kernel headers.
- Longer term: a live pipeline into Monado (see [[quest1-open-vr-project]]). Note that replacing
  Meta's `trackingservice` in-place is a *separate, much larger* project — it also does controller
  constellation tracking and publishes poses through an un-RE'd binder + ashmem contract.

### Things that cost iterations (record so they aren't repeated)
- **`pgrep -f` / `pkill -f <pattern>` matches the very shell running it.** `pkill -9 -f
  sensors@1.0-service` killed our own `su` shell (exit 137), and a poll loop using
  `pgrep -f` never saw the service as down because the `pgrep` subshell matched itself. This
  produced a bogus "orphan HAL process" theory. **Use `pidof <exe-name>` instead.**
- `/dev/video0` is **single-open** (`msm.c:1091`), and `stop`ping the HAL alone doesn't hold —
  the same process serves `android.hardware.sensors@2.0::ISensors`, so the framework pulls it
  back. Must `stop` the framework too.
- `qcamera_get_sensor` is an **acquire**, not a getter: a second call returns NULL until
  `qcamera_release_sensor`. `qcamera_close` asserts "Cameras still in use!" if you don't release.
- Several `qcamera_*` functions return **void** — the "rc" is garbage; judge by out-params.
- `qcamera_start_sensor` adds the meta row itself: pass the **640×480** dims, not 640×481, or
  the sensor comes up 640×482.
- The 28-byte cfg only needs `cfg[0] = 112` (cam_format for fourcc `'GREY'` mono8; **not** 42 —
  42 passes `mm_stream_calc_offset_raw`'s range check but has no V4L2 mapping, giving
  `Unknown fmt=42` and no frames). `p4` can be **NULL** — the lib allocates its own buffers
  (`+0x50`/`+0x58` get populated).

### Earlier stages (superseded by the live result above)
Stage 1 `enum` already touches hardware: `qcamera_open` → `control_init` opens `/dev/media*`,
so the sensors HAL (holding `/dev/video0`) must be stopped first. Stages 2/3 dump the sensor
object and the 80-byte frame container raw, to pin `cfg`/`p4` and locate the pixel pointer and
exposure timestamp empirically in one run — validate pixels against
`exports/ib-capture-2026-09-01/`.

## Reversibility
This probe changed nothing (sysfs/procfs reads + a host-side git clone). Step 3 is the first
step that touches device state, and it requires stopping the HAL — restart-cascade hazard from
[[10-CHECKPOINT-camera-tap]] applies.
