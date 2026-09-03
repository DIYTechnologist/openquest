# Reference ioctl trace for B2 — 2026-09-03

Step 1.1 of `notes/18`, **done**. B2 reimplements `libqcameradriver.so` against the published kernel
headers; this is the reference trace to reimplement *against* — the exact device topology, call
ordering and payloads that a working 4-camera streaming session produces.

Tooling: `tools/cam_kernel/` (`ioctl_trace.c`, `build.sh`, `gen_ioctl_table.sh`,
`run_ioctl_trace.sh`). Data: `exports/ioctl-trace-2026-09-03/ioctl_trace.log` — 493 ioctls from a
`cam_direct all 0 3 3000 160` run, all 4 cameras streaming. Device restored cleanly.

**Every ioctl in the trace is now decoded by name.** No opaque codes remain.

## The pipeline, as actually driven

| Device | Role | ioctls |
|---|---|---|
| `/dev/media0`–`media4` | graph discovery | `MEDIA_IOC_ENUM_ENTITIES` ×101 on media0, `DEVICE_INFO` |
| `/dev/video0` | msm-config | `MSM_CAM_V4L2_IOCTL_DAEMON_DISABLED` ×1 |
| `/dev/ion` | buffers | `ION_IOC_ALLOC`/`SHARE`/`FREE` ×36 each |
| `/dev/v4l-subdev0,1,2` | **CSIPHY** ×3 | `VIDIOC_MSM_CSIPHY_IO_CFG` ×20, `SENSOR_GET_SUBDEV_ID` |
| `/dev/v4l-subdev3,4,5,6` | **CSID** ×4 | `VIDIOC_MSM_CSID_IO_CFG` ×20, `SENSOR_GET_SUBDEV_ID` |
| `/dev/v4l-subdev12` | **ISPIF** | `VIDIOC_MSM_ISPIF_CFG` ×14, `CFG_EXT` ×4 |
| `/dev/v4l-subdev10,11` | **ISP/VFE** ×2 | `REQUEST_STREAM`, `CFG_STREAM`, `INPUT_CFG`, `SMMU_ATTACH`, `AHB_CLK_CFG`, `ENQUEUE_BUF`/`DEQUEUE_BUF`, `UPDATE_STREAM`, `SUBSCRIBE_EVENT` |
| `/dev/video3,4,5,6` | one per camera | `S_FMT`, `S_PARM`, `REQBUFS`, `QBUF` ×7, `DQBUF` ×3, `STREAMON`/`STREAMOFF`, `G_CTRL` |

That is the textbook msm8998 path — **sensor → CSIPHY → CSID → ISPIF → VFE → video node** — and
every ioctl on it is in the published tree. Nothing proprietary appears anywhere in the trace,
which is the key result: **B2 has no unknown-ABI blocker.**

Note the counts: 3 CSIPHY and 4 CSID subdevs for 4 sensors, and **2** VFEs shared across the four
video nodes. The two ISP subdevs receive identical call sequences, so streams are split across
them.

## Tooling notes, all of them bugs worth not repeating

1. **`dlsym(RTLD_NEXT)` + stdio in an LD_PRELOAD constructor segfaults instantly.** A constructor
   calling `fopen()` re-enters our own `open()` wrapper before `dlsym` has resolved the real one.
   Everything now goes through raw `syscall()`; there is no bootstrap ordering left to get wrong.
2. **Interposing `open`/`openat` silently captures nothing.** bionic inlines both to `__openat`
   under `_FORTIFY_SOURCE`, so the vendor library never calls the symbols we replaced — every fd
   logged as `?`, losing exactly the information the trace exists to capture. Resolve paths from
   `/proc/self/fd/<fd>` instead; it is immune to how the fd was created.
3. **Do not cache fd → path.** fds are closed and reused, so a cached path goes stale: the trace
   showed `ION_IOC_ALLOC` arriving on `/dev/v4l-subdev0`, which is impossible, and that impossible
   line is the only reason the bug was caught. Re-read the link every call — ~500 extra syscalls
   across a whole capture.
4. **bionic declares `ioctl(int, int, ...)` and marks it overloadable**, so the interposer must
   match that signature exactly or it will not compile.
5. **Do not put `$K/include/uapi` on the include path.** bionic's own `#include <linux/types.h>`
   then resolves into the kernel tree and redefines `struct sigaction`. Stage only `media/*.h` plus
   the two `linux/` headers the NDK lacks (`media.h`, and `ion.h` from
   `drivers/staging/android/uapi/`).

`gen_ioctl_table.sh` harvests ioctl *macros* from the kernel headers so the compiler computes every
code — no number is transcribed by hand, and the table cannot drift from the headers.

## Next in step 1

1.2 MCU control: replace `libsyncboss.so` with raw `/dev/syncboss0` writes (type 40 on / 41 off;
`cam_direct` already emits 41). 1.3 Reimplement the pipeline above. 1.4 Parity against B1 per the
`notes/18` acceptance criteria.

The payload hex is captured for every call, so the struct-level decode for 1.3 can be done offline
against the headers — no further device time is needed to design the reimplementation.

## Update: pointer-following, and the CSI configuration (step 1.3 design input)

`csid_cfg_data` and `csiphy_cfg_data` are only 16 bytes on the wire — `{ u32 cfgtype, union }` —
and for the `*_params` cfgtypes the union is a **pointer**. The first version of the trace captured
the pointer value and none of the actual lane/clock/decode configuration, while looking complete.

Following it needs care: the same union holds a plain `u32` for the version query (cfgtype 0
returns `csid_version = 0x50000000`, i.e. CSID v5.0), and dereferencing *that* as an address
segfaults the whole capture — which is what happened on the first attempt. The tracer now reads
through `process_vm_readv`, which returns `-EFAULT` instead of dying, so no cfgtype table is needed
and an unexpected one cannot crash the run.

Recovered configuration, per camera:

| | value |
|---|---|
| CSIPHY | `lane_cnt=1`, `lane_mask=0x0e` |
| CSID | `lane_cnt=1`, `lane_assign=0x4320`, 1 CID entry (`vc/dt` = `01 00 00 00`) |
| `phy_sel` per camera | **0, 1, 2, 2** |

So each camera runs a **single CSI lane**, and the four sensors map onto **three CSIPHYs** with two
sharing phy 2 — consistent with the 3 CSIPHY / 4 CSID / 2 VFE subdev counts in the topology above.

### Per-camera bring-up order (what 1.3 must replay)

```
G_CTRL(videoN) -> CSID_IO_CFG(version query) -> CSIPHY_IO_CFG
ION alloc/share xN -> S_PARM -> S_FMT -> REQBUFS(count=4, type=9 PRIVATE, memory=2 USERPTR)
QBUF x4 -> STREAMON
CSIPHY_IO_CFG(params) -> CSID_IO_CFG(params)
ISP: [AHB_CLK_CFG once per VFE] SMMU_ATTACH -> INPUT_CFG -> REQUEST_STREAM -> SUBSCRIBE_EVENT
     -> REQUEST_BUF -> ENQUEUE_BUF x4 -> UPDATE_STREAM -> CFG_STREAM
ISPIF: ISPIF_CFG -> ISPIF_CFG_EXT -> ISPIF_CFG
```

VFE assignment is cameras 0,1 → `v4l-subdev10` and cameras 2,3 → `v4l-subdev11`.
`ISP_REQUEST_STREAM` carries `session=3, stream=1, 'GREY'` (`V4L2_PIX_FMT_GREY`), and `S_FMT`
reports `sizeimage=307840` = 640×481 mono8, matching what the frames actually are.

### Still to decode for 1.3

`msm_vfe_input_cfg` (172 B), `msm_vfe_axi_stream_request_cmd` (144 B) and `msm_ispif_cfg_data`
(368 B). All three are captured in full in the trace and all three structs are in the published
headers, so this is offline work — no further device time is needed to design the reimplementation.

## Full decode: the B2 configuration in named parameters (step 1.3 design COMPLETE)

`tools/cam_kernel/decode_payloads.c` compiles against the same published headers the device uses,
so field offsets come from the compiler rather than from counting bytes. Every parameter B2 needs
is now a named value rather than a captured byte blob:

**ISPIF** (`/dev/v4l-subdev12`)

| cfg_type | payload |
|---|---|
| `SET_VFE_INFO` (10) | `num_vfe = 2` |
| `INIT` (2) | `csid_version = 0x50000000` (CSID v5.0) |
| `CFG` (3) | 1 entry: `vfe_intf=0, intftype=RDI0, num_cids=1, cids=[0], csid=0, crop_enable=0` |
| `START_FRAME_BOUNDARY` (4) | same entry |
| `STOP_IMMEDIATELY` (7) | same entry |

**ISP / VFE** (`/dev/v4l-subdev10`, `11`)

```
INPUT_CFG      input_src = VFE_RAW_0, input_pix_clk = 48 000 000
               rdi_cfg { cid = 0, frame_based = 1 }
REQUEST_STREAM session = 3, stream = 1, output_format = 'GREY',
               stream_src = RDI_INTF_0, frame_base = 1, init_frame_drop = 0
```

`plane_cfg` is all zeros — for a frame-based RDI stream the plane geometry is unused, which is why
`sizeimage` comes from `S_FMT` instead.

**CSI** (from the pointer-follow above): CSIPHY `lane_cnt=1, lane_mask=0x0e`; CSID `lane_cnt=1,
lane_assign=0x4320`, one CID; `phy_sel` 0,1,2,2 across the four cameras.

**Video node**: `REQBUFS count=4, type=9 (V4L2_BUF_TYPE_PRIVATE), memory=2 (USERPTR)`, `QBUF` ×4,
`STREAMON`.

### The shape of the answer

The Quest drives its tracking cameras through the **RDI (raw dump) path, not CAMIF** —
`stream_src=RDI_INTF_0`, `input_src=VFE_RAW_0`, `intftype=RDI0`, `frame_based=1`. That is the
simplest path the VFE offers: no demosaic, no scaling, no statistics, just CSI frames written
straight to memory. For B2 that is very good news — the entire ISP pixel-processing configuration,
which is where the vendor complexity would normally live, is simply not used.

Per-camera the only things that vary are the subdev indices, `phy_sel`, `csid`, `vfe_intf`
(cameras 0,1 → VFE0; 2,3 → VFE1) and the RDI interface number. Everything else is constant.

### Union decoding: the same trap twice

`ispif_cfg_data`'s union member depends entirely on `cfg_type`. Reading it as `params`
unconditionally made `ISPIF_INIT` report `num=1342177280`, which is really
`csid_version=0x50000000` — the same value that, one bug earlier, was being dereferenced as a
pointer. **Decode the tag before the member.** Both mistakes produced confident, wrong output
rather than an error.
