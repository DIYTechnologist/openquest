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

> **CORRECTED below.** The first pass here read `lane_mask=0x0e`; that is wrong — `0x0e` is
> `settle_cnt`, decoded by eye. See the compiler-checked table further down.

Each camera runs a **single CSI lane**, and the four sensors map onto **three CSIPHYs** with two
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


## CSI parameters, compiler-decoded and gated on cfgtype

| cam | CSIPHY | CSID |
|---|---|---|
| 0 | `lane_cnt=1 settle_cnt=14 lane_mask=0x0003 combo_mode=0 csid_core=0` | `lane_cnt=1 lane_assign=0x4320 phy_sel=0 num_cid=1` |
| 1 | same, `csid_core=1` | `lane_assign=0x4320 phy_sel=1` |
| 2 | same, `csid_core=2` | `lane_assign=0x4320 phy_sel=2` |
| 3 | `lane_mask=0x0018 combo_mode=1 csid_core=3` | `lane_assign=0x0003 phy_sel=2` |

**Camera 3 shares CSIPHY 2 with camera 2 via combo mode** (`combo_mode=1`, a different
`lane_mask`, and a different `lane_assign`). That is what the 3-PHY / 4-camera mapping actually
means, and it is the one place the per-camera configuration is not a simple index substitution.

`csiphy_clk`, `csi_clk` and `data_rate` are all zero — the sensors are FSIN slaves clocked by the
MCU, so userspace does not program a rate here.

### Known gap

`csid_params.lut_params.vc_cfg[]` is an array of **pointers** into the traced process, so the
per-CID `vc`/`dt`/`decode_format` values are not recoverable from this trace; `ioctl_trace` would
have to chase them at capture time. `num_cid=1` for every camera. Following them in the decoder
segfaulted it, which is how the gap was found.

### The recurring trap, four times over

Every one of these structures is a **tagged union**, and four separate bugs came from reading the
member before the tag:

1. `csid_cfg_data` cfgtype 0 holds `csid_version 0x50000000`; dereferencing it as a pointer
   segfaulted the capture.
2. `ispif_cfg_data` read as `params` made `ISPIF_INIT` report `num=1342177280` — the same
   `0x50000000`.
3. `csiphy_cfg_data` on the `INIT` calls decoded as params gave `lane_cnt=32, settle_cnt=67,
   data_rate=9369376207381987455`.
4. `lut_params.vc_cfg[]` pointers, above.

Case 3 is the instructive one: those values are absurd enough to notice, but nothing *forced*
noticing them. Cases where a mis-tagged union yields plausible numbers are the ones that ship.
**Read the tag, then the member — every time.**

## B2 implementation status — pipeline up, CSI flowing, one bug left

`tools/cam_kernel/cam_kernel.c` brings the whole pipeline up with **zero Meta blobs** — no
`libqcameraoculushal.so`, no `libqcameradriver.so`, no `libsyncboss.so`:

```
[+] discovered: 3 csiphy, 4 csid, 2 vfe, ispif=/dev/v4l-subdev12, 4 video
[+] mcu camera_probe / set_bpp / camera_init / set_frame_rate / set_exposure_gain / tag_mode
[+] cam0 session=3  (/dev/video3, /dev/v4l-subdev3, /dev/v4l-subdev0, vfe0)
[+] cam0 pipeline up          <- S_PARM, S_FMT, REQBUFS, QBUF x4, STREAMON,
[+] 1/1 camera pipelines up      CSIPHY_CFG, CSID_CFG, SMMU_ATTACH, INPUT_CFG,
[+] mcu start_streaming <-- FSIN  REQUEST_STREAM, REQUEST_BUF, ENQUEUE_BUF x4,
                                  UPDATE_STREAM, CFG_STREAM, ISPIF_CFG, ISPIF_START
```

The kernel confirms CSID initialises:

```
msm_csid_init: CSID_VERSION = 0x30050000
msm_csid_irq  CSID0_IRQ_STATUS_ADDR = 0x800
```

> **CORRECTION.** An earlier version of this note read that IRQ as "data is arriving". It is not.
> `msm_csid.c` completes `reset_complete` on `csid_rst_done_irq_bitshift`, and `0x800` is exactly
> that bit — the interrupt is a **reset acknowledgement**, not a packet. There is still no evidence
> of CSI traffic from the sensors.

**The one remaining bug**, and the kernel names it exactly:

```
msm_isp_cfg_ping_pong_address: msm_isp_get_stream_buffer() returned null, configuring scratch
```

The ISP has no buffers bound to the stream at ping-pong programming time, so it writes each frame
into a scratch buffer instead of ours — hence `DQBUF` never produces anything. `ISP_REQUEST_BUF`
and `ISP_ENQUEUE_BUF` both return 0, so the buffers are being *registered* but not *associated with
the stream*; the suspect is the bufq binding done by `ISP_UPDATE_STREAM /
UPDATE_STREAM_ADD_BUFQ`, specifically whether `user_stream_id` must match the video node's
fh-assigned stream id rather than the `1` used in `REQUEST_STREAM`.

### The decode error that cost a cycle

`S_FMT`/`REQBUFS`/`QBUF` use `type = 9`, which the first decode pass called
`V4L2_BUF_TYPE_PRIVATE`. It is not — **`V4L2_BUF_TYPE_PRIVATE` is `0x80`; 9 is
`V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE`.** With PRIVATE, `S_PARM` failed `EINVAL` inside the v4l2
core's `check_fmt()`, which rejects PRIVATE unless the driver exports
`vidioc_g_fmt_type_private` (`camera.c` does not). MPLANE also changes the buffer layout: `m.planes`
is a pointer to a `v4l2_plane` array and `length` is the **plane count**, not a byte count — which
is exactly what the traced `QBUF` showed (`length=1` alongside a pointer).

### Two operational lessons

- **Line-buffer stdout.** Running detached with output redirected, the default block buffering meant
  the first hang produced a completely empty log.
- **Bound the tool's own lifetime.** A hang here is worse than a crash: this process holds
  `/dev/video*` and `/dev/syncboss0`, which are single-open, so `trackingservice` and the sensors
  HAL sat in a "restarting" loop until it was killed by pid. The restore watchdog cannot help —
  it restarts services, it does not kill the holder. `cam_kernel` now sets `alarm(secs + 30)` and
  opens the video nodes `O_NONBLOCK`, polling before `DQBUF`.

## Buffer binding solved: the video node must be opened TWICE

`msm_isp_get_stream_buffer() returned null, configuring scratch` is fixed, and the cause is worth
recording because every ioctl involved returned success throughout.

`S_PARM` registers the vb2 queue under `(session, fh stream_id)`, while `ISP_REQUEST_BUF` creates
the ISP bufq under whatever ids we pass. If those disagree the bufq is still created, `ADD_BUFQ`
still binds it, and every call still returns 0 — but `msm_isp_get_buf()` finds no vb2 queue for the
pair and the ISP silently writes every frame into a scratch buffer.

The correct stream id is **not** a free choice:

- `camera_v4l2_open()` calls `camera_v4l2_fh_open()` **before** marking the node opened, and
  fh_open does `stream_id = find_first_zero_bit(&atomic_read(&pvdev->opened))`.
- So the **first** handle on a node gets `stream_id 0` *and creates the msm session*; the **second**
  gets `stream_id 1`.
- `ISP_REQUEST_BUF` rejects `stream_id 0` outright with `EINVAL` — which is the tell that 0 is not
  usable, and why the vendor trace shows 1.

So the vendor opens each video node twice and streams on the second handle. `cam_kernel` now does
the same (`vfd_session` holds the first open, `vfd` is the streaming handle), and the scratch-buffer
fallback is gone.

## Where B2 actually stands

| stage | state |
|---|---|
| media-graph / subdev discovery | works (via sysfs names) |
| MCU: power, bpp, init, frame rate, exposure/gain, tag mode, FSIN start | works, blob-free |
| video node: dual open, S_PARM, S_FMT, REQBUFS, QBUF, STREAMON | works |
| CSIPHY_CFG, CSID_CFG | accepted; CSID resets and reports v0x30050000 |
| ISP: SMMU_ATTACH, INPUT_CFG, REQUEST_STREAM, REQUEST_BUF, ENQUEUE_BUF, UPDATE_STREAM, CFG_STREAM | works; buffers correctly bound |
| ISPIF: SET_VFE_INFO, INIT, CFG, START_FRAME_BOUNDARY | accepted |
| **frames delivered** | **none** |

Every ioctl returns 0 and nothing in the kernel log complains any more — but no CSI packets arrive.
Next suspects, in order:

1. **`VIDIOC_MSM_ISPIF_CFG_EXT` is not being sent.** The vendor's per-camera ISPIF sequence is
   `CFG → CFG_EXT → START_FRAME_BOUNDARY`; `cam_kernel` currently sends `CFG → START`.
2. **`lut_params.vc_cfg`** — the per-CID `vc`/`dt`/`decode_format` values were never captured (they
   live behind pointers, see the gap above). `cam_kernel` guesses `dt=0x2a` (RAW8) and
   `decode_format=1`. If the sensors emit a different data type, CSID will drop every packet
   silently, which matches the symptom exactly. **This is the most likely cause.**
3. Sensor-side subdev configuration (`VIDIOC_MSM_SENSOR_GET_SUBDEV_ID` probes appear in the vendor
   trace and are not replicated).

Closing (2) means extending `ioctl_trace` to chase `vc_cfg[]` at capture time — cheap, and it turns
a guess into a measurement.

## B2 status: sequence complete, still no frames — ordering is the open lead

Since the last update, four candidate causes were investigated. Three are **eliminated**, one is
**fixed**, and the real difference now looks structural.

**Eliminated — the CSI data type was already right.** Extended `ioctl_trace` with a second-level
pointer chase for `lut_params.vc_cfg[]` (`msm_camera_csid_params` has an inline `vc_cfg_a[]` at
offset 17 *and* a pointer array `vc_cfg[]` at 72; the vendor leaves the inline array zeroed and
fills the pointers, so the values were genuinely missing from the first capture). Measured:
`cid:0, dt:0x2a, dec:1` — exactly what `cam_kernel` was already guessing. A guess became a
measurement and a suspect was removed, which is worth more than it sounds.

**Eliminated — `ISPIF_CFG_EXT`.** The vendor's third ISPIF call is `cfg_type=11` (`ISPIF_CFG2`) via
`VIDIOC_MSM_ISPIF_CFG_EXT`, whose `{cfg_type, void *data, u32 size}` points at a 724-byte
`msm_ispif_param_data_ext`. Chased and decoded: the same single entry as `CFG`, `pack_cfg[]` all
zeros, stereo disabled. Implemented; no change.

**Eliminated — `AHB_CLK_CFG` and the subdev-id probes.** Both were missing (found by diffing
`cam_kernel`'s own trace against the vendor reference — the debugging loop this tooling was built
for). `AHB_CLK_CFG` votes `2`. Implemented; no change.

**Fixed — `CSIPHY_INIT` was never sent.** Counting cfgtypes in the vendor trace: `CSIPHY_INIT` ×8,
`CSIPHY_CFG` ×4, `CSIPHY_RELEASE` ×8 — while `cam_kernel` sent only `CSIPHY_CFG`. Payload is an
all-zero union. Implemented; still no frames, so it was necessary-but-not-sufficient.

### The open lead: bring-up ORDER differs structurally

An ordered diff of the first-camera sequence shows the two flows are not the same shape:

| vendor | cam_kernel |
|---|---|
| `DAEMON_DISABLED` | `DAEMON_DISABLED` |
| **INIT sweep across *all* CSIPHY/CSID subdevs up front** | ISPIF `SET_VFE_INFO`, `INIT` |
| per-camera `G_CTRL`, `CSID_CFG`, `CSIPHY_CFG` | `G_CTRL`, `CSID_INIT` |
| `S_PARM`, `S_FMT`, `REQBUFS`, `QBUF`×4, `STREAMON` | `S_PARM`, `S_FMT`, `REQBUFS`, `QBUF`×4, `STREAMON` |
| **`ISPIF_CFG` ×2** | `CSIPHY_INIT`, `CSIPHY_CFG`, `CSID_CFG` |
| `AHB_CLK_CFG` | `AHB_CLK_CFG`, subdev-id probes |
| **`CSIPHY_CFG`, `CSID_CFG` *again*, after ISPIF** | ISP block |
| ISP block | ISPIF `CFG`, `CFG2`, `START` |

Two concrete differences worth trying, in order:

1. **Initialise every CSIPHY and CSID up front**, before any per-camera configuration, rather than
   lazily per camera.
2. **Move `ISPIF_CFG` before the ISP block, and re-send `CSIPHY_CFG`/`CSID_CFG` after it.** The
   vendor configures the PHY/CSID *twice*, straddling the ISPIF routing setup — plausible if the
   PHY must be programmed once the ISPIF path is established.

Everything returns 0 in both flows, so ordering is exactly the kind of difference that produces
this symptom: a correctly-configured pipeline that never receives a packet.

The diff harness itself is the deliverable here — `run_ck_trace.sh` runs `cam_kernel` under
`libioctl_trace.so`, and comparing the two traces by count *and* by position is what localised
every one of the four items above.

## B2: kill criterion reached — parked, with B1 as the working fallback

Both ordering changes from the previous section were implemented, plus a third fix found along the
way. None produced frames, so per the `notes/18` kill criterion this is parked rather than ground
on further.

**Implemented since the last update**

1. **Global CSIPHY/CSID INIT sweep up front**, before any per-camera configuration — matching the
   vendor's shape rather than initialising lazily per camera. This matters on this rig because
   cameras share PHYs (camera 3 runs combo-mode on CSIPHY 2), so a lazily-initialised PHY can be
   configured before it is ready.
2. **CSI configuration moved after `STREAMON` and `AHB_CLK_CFG`**, before the ISP block, as the
   vendor does.
3. **Subdev fds are now held open for the whole session.** The first version of the sweep opened
   each subdev, sent INIT, and closed it — which undoes the init immediately, because the CSID
   driver releases on last close (`msm_csid_release` in dmesg). A real bug, and worth keeping fixed.

Result: pipeline still comes up cleanly, every ioctl returns 0, kernel log is clean, **no frames**.

### The concrete unexplained discrepancy to start from

`CSID_INIT` behaves differently for us than for the vendor, and this is the sharpest remaining
thread:

| | vendor | cam_kernel |
|---|---|---|
| ioctl return | **10** | 0 |
| `cfg.csid_version` written back | **0x50000000** | **0x00000000** |

The kernel does `rc = msm_csid_init(csid_dev, &cdata->cfg.csid_version)` (`msm_csid.c:707`) and
dmesg confirms the hardware read succeeds (`msm_csid_init: CSID_VERSION = 0x30050000`) — yet the
value never reaches our struct, and the vendor gets a non-zero return code of 10 where we get 0.
Struct layout is not the explanation: `csid_cfg_data` puts the union at offset 8 on both sides, and
the vendor's own captured payload carries the version there.

Something about *how* the vendor reaches this call differs from ours. That is where to resume.

Also noted: the vendor's second CSID call is `cfgtype=3` = **`CSID_RELEASE`**, not another config —
so its CSID lifecycle is init/release-heavy in a way we have not reproduced (`CSID_INIT` ×8,
`CSID_RELEASE` ×8, `CSID_CFG` ×4 across the session).

### Where this leaves the project

- **B1 still works** and remains the camera path for all stock-OS work (steps 2–4 of `notes/18`).
  Nothing downstream is blocked by B2 being incomplete.
- **B2 is only required for the OS swap** (`notes/16`: no vendor partition, so `/vendor` does not
  survive). It is worth resuming at that point anyway, when the kernel is being rebuilt and the
  driver side can be instrumented directly instead of inferred from a userspace trace — which is a
  far better position than reverse-engineering the sequence from outside.
- Everything needed to resume is committed: the reference trace with all payloads decoded, the
  `cam_kernel` implementation, and the trace-diff harness (`run_ck_trace.sh`) that localises
  divergence by position, not just by count.
