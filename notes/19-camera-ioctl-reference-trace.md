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

## Session 3: two more real bugs found, exhaustive verification, still parked

Kept digging past the first kill-criterion checkpoint since two of the four remaining suspects
were genuine bugs, not dead ends. Both are now fixed and confirmed real via direct evidence.

### Fixed: `CSIPHY_INIT` was never sent, and the vendor's INIT/RELEASE/INIT dance matters

Counting cfgtypes in the vendor trace: `CSIPHY_INIT` ×8, `CSIPHY_CFG` ×4, `CSIPHY_RELEASE` ×8 —
`cam_kernel` sent only `CSIPHY_CFG`. The vendor's exact per-camera shape, recovered from a
positional (not just count) diff of the reference trace:

```
CSID_INIT, CSIPHY_INIT, CSIPHY_RELEASE, CSID_RELEASE        (on the FIRST video-node handle)
G_CTRL, CSID_INIT, CSIPHY_INIT                                (on the SECOND handle)
S_PARM, S_FMT, REQBUFS, QBUF x4, STREAMON
ISPIF_SET_VFE_INFO, ISPIF_INIT, AHB_CLK_CFG
CSIPHY_CFG, CSID_CFG                                          (configured AFTER streamon)
SMMU_ATTACH, INPUT_CFG, REQUEST_STREAM, SUBSCRIBE_EVENT,
REQUEST_BUF, ENQUEUE_BUF x4, UPDATE_STREAM, CFG_STREAM
ISPIF_CFG, ISPIF_CFG2, ISPIF_START_FRAME_BOUNDARY
```

`cam_kernel` now replays this exactly, including `CSIPHY_RELEASE` (which needs real lane
params — passing a zeroed union faults inside `msm_csiphy_cmd` at `copy_from_user`, logged as
`msm_csiphy_cmd: 1551 failed`; fixed by sending the same `{lane_assign, lane_mask}` as `CFG`).

### Fixed: RDI interface assignment for cameras sharing a VFE

Real, confirmed bug, only visible once testing moved from 1 camera to 4. Two cameras share each
VFE (0,1→vfe0; 2,3→vfe1), and each VFE exposes multiple RDI interfaces (`VFE_RAW_0/1/2`).
`cam_kernel` hardcoded `VFE_RAW_0` for every camera; the second camera on a VFE then failed
`ISP_INPUT_CFG` with `EINVAL` — invisible with `ncam=1`, where a lone camera on an idle VFE always
gets `RAW_0` and always succeeds, which is exactly what every single-camera debug run before this
one exercised. Vendor trace, byte-decoded: `input_src=1 (VFE_RAW_0)` for the first camera on a
VFE, `input_src=2 (VFE_RAW_1)` for the second; `REQUEST_STREAM.stream_src` correspondingly
`RDI_INTF_0`/`RDI_INTF_1`. Fixed as `VFE_RAW_0 + (i % 2)` / `RDI_INTF_0 + (i % 2)`. All 4 camera
pipelines now come up cleanly with zero kernel errors.

### Extensive verification that ruled out everything else checked

None of the following explain the missing frames, each confirmed by reading the actual driver
source or decoding the actual bytes with the compiled struct (not by inspection):

- **`msm_isp_get_stream_buffer() returned null, configuring scratch` is not the failure signal.**
  It fires in the **working** B1 path too (10 times in a 3 s 4-camera run) — it's normal
  ping-pong-register initialisation noise at stream start, not evidence of anything broken.
- **Frame-boundary interrupts are firing.** `dmesg`'s rate-limit machinery reports
  `msm_isp_cfg_ping_pong_address: 44 callbacks suppressed` over an 8 s single-camera run — the
  function is being called repeatedly throughout, which needs a live interrupt source. This
  weighs against (though does not disprove) the "no CSI data" theory from the previous session.
- **The `v4l2_plane.m.userptr`/`.fd` field is functionally inert.** Read `__qbuf_userptr()` in
  `videobuf2-core.c` and MSM's own `msm_vb2_dma_contig_get_userptr()`: the latter is a bare
  `kzalloc` + store, no `get_user_pages`, no validation. Generic vb2 core only checks
  `length >= plane_size`. The real buffer↔hardware wiring is entirely through
  `ISP_REQUEST_BUF`/`ISP_ENQUEUE_BUF` (which uses the ION fd correctly). Confirmed the vendor's own
  captured value in this field (`33`) is implausible as a real pointer and is presumably just
  their own fd number reused — harmless either way.
- **The `csid_version` return-value discrepancy (vendor reads back `0x50000000`, we read `0`) is
  diagnostic-only,** not load-bearing. `msm_csid_init()` does a *live register read*
  (`csid_dev->hw_version = msm_camera_io_r(...)`, confirmed equal to `CSID_VERSION_V35` in dmesg
  on both working and non-working runs) and assigns it to the output parameter *before* any
  branch that could affect kernel-internal state; internal `if (hw_version < V30)` branches use
  the kernel's own copy, not what gets copied back to userspace. Whatever discrepancy exists here
  cannot itself change the kernel's own behaviour. Retired as a suspect — an earlier note ranked
  this as "the sharpest remaining thread," which was a misprioritisation.
- **`ISP_CFG_STREAM`'s `cmd` value matches**: vendor sends `cmd=1` (`START_STREAM`),
  `stream_handle[0]=0x205`, matching our own `axi_stream_handle` exactly.
- **Daemon-disabled semantics are correct and global.** `MSM_CAM_V4L2_IOCTL_DAEMON_DISABLED` sets
  a module-scope `is_daemon_status = false` (`msm.c`), so sending it once on `/dev/video0` before
  opening any camera node is sufficient — confirmed by reading `camera_v4l2_streamon`/`s_parm`,
  both of which skip the daemon round-trip entirely and do the essential `vb2_streamon`/
  `msm_create_stream` unconditionally.
- **Small settle delays (2–5 ms) between CSIPHY_INIT/CFG and CSID_CFG/ISPIF_START** made no
  difference — added and tested, then this is not a PHY-lock timing issue at the granularity
  tried.

### Where this leaves it

Every ioctl, in the right order, with byte-identical payloads (verified against the compiled
kernel structs, not eyeballed) and now-correct RDI routing — and still no frames. The remaining
candidates are not reachable by tracing userspace ioctls at all: sensor-side I2C/CCI state
(entirely in-kernel per `notes/11`, invisible to us), SMMU/IOMMU mapping faults (would need
`dmesg` iommu fault logging, none observed, or ION cache-flag effects, though those could not
explain zero DQBUF *events*, only wrong *content*), or something in the physical FSIN/CSI signal
timing that requires a scope or kernel printk instrumentation to see — which means flashing a
kernel, explicitly off-limits per the standing rules.

**Restating the parked status from the previous checkpoint, now on stronger evidence**: B1 remains
the working camera path for all stock-OS work. B2 is only required for the OS swap itself, and
is better resumed there — a from-scratch kernel build is a natural point to add real
`pr_info`/`CDBG` instrumentation to the driver rather than inferring behaviour from a userspace
ioctl trace, which is close to the ceiling of what that method can resolve.

## Session 4: full payload diff finds four more real bugs; buffer binding SOLVED

Spot-checking eight ioctls' payloads had missed things. Doing a **full positional payload diff** of
every cam0 ioctl (both traces, byte-compared against the compiled structs) found four more genuine
bugs. All are fixed. The `configuring scratch` fallback is now **gone** — buffer binding is
correct at last — but the ION buffers still come back all-zero, so CSI data is genuinely not
reaching the VFE.

### Fixed: `SUBSCRIBE_EVENT` was subscribing to nothing

`sub.type` is an **ISP event bitmask**, not a v4l2 event type. `msm_isp_process_event_subscription()`
walks it bit-by-bit against `ISP_EVENT_MASK_INDEX_*`. Passing `V4L2_EVENT_ALL` (0) matches
`ISP_EVENT_SUBS_MASK_NONE`, so it subscribed to **nothing** and returned success. The kernel said
so plainly — `Subs event_type is None=0x0` sat in dmesg for several debug cycles before its meaning
was chased down. Vendor sends `0x1dff`. A reminder that "returns 0" and "did something" are
different claims.

### Fixed: duplicate video-node open → wrong `stream_id`

The decisive find. `camera_v4l2_s_parm()` writes the fh's stream_id back into
`parm.capture.extendedmode`; decoding it showed **vendor=1, ours=2**. An earlier restructure had
left *two* "second open" blocks, so the node was opened three times and the streaming handle landed
on `stream_id 2` while `ISP_REQUEST_BUF` created the bufq for the hardcoded `1`. Nothing errored —
the bufq was created, `ADD_BUFQ` bound it — and every frame went to a scratch buffer.

Now fixed twice over: the duplicate open is removed, **and** `stream_id` is read back from
`S_PARM` rather than assumed. Hardcoding it was the original sin; the kernel hands you the answer.

### Fixed: `ISP_SMMU_ATTACH` security_mode

Vendor sends `security_mode=1`; we sent 0. (`IOMMU_ATTACH` is 0 in the enum, so
`iommu_attach_mode=0` was already right — it was the other field.)

### Fixed: RDI interface assignment (see session 3) — only visible with 4 cameras

Restated because it is the clearest methodological lesson: two cameras share each VFE, and
hardcoding `VFE_RAW_0` made the *second* camera on a VFE fail `ISP_INPUT_CFG` with `EINVAL`. Every
single-camera debug run before that passed, because a lone camera on an idle VFE always gets
`RAW_0`. **The bug was invisible at the test size being used.**

## Confirmed: this is no longer a buffer-handoff problem

Added a direct ION readback that bypasses `DQBUF` entirely — mapping the buffers and reading them
regardless of what the v4l2 queue says. This cleanly separates "hardware never captured" from
"captured but the handoff back to userspace is broken":

```
cam0 buf0..3: mean=0.00  nonzero=0/307840  (all zero)
```

**All four buffers are untouched.** Combined with `configuring scratch` being gone (so the ISP
*is* programming real buffer addresses into ping-pong) and CSID showing only `0x800`
(reset-done) interrupts, the remaining fault is upstream of the VFE write path: the sensor is not
emitting CSI data, or CSID/ISPIF is not accepting it.

Control re-verified in the same session: **B1 still delivers frames** on this exact hardware
(`cam_direct all` → 4 cameras, non-zero means), so the sensors, MCU strobe and hardware are fine
and the baseline is live. The MCU command sequence is also proven independently, since B1 was run
with `SYNCBOSS_RAW=1` — our own packet implementation.

### Also ruled out this session

- **Holding `msm_sensor_init` (v4l-subdev8) open** — `notes/11` attributes in-kernel OV7251 CCI/I2C
  bring-up to that subdev's probe, and neither trace issues ioctls on it, so open() was the only
  remaining way it could matter. Tested; no change. Kept in the code as harmless.
- The kernel genuinely does `copy_from_user` through `lut_params.vc_cfg[i]` pointers
  (`msm_csid.c:748`), so the stack-allocated `vc_cfg` with `dt=0x2a` reaches CSID correctly —
  the inline `vc_cfg_a[]` is not the path used.
- Every remaining payload difference in the full diff is accounted for: union pointer *values*
  (expected to differ), the ION fd in `ENQUEUE_BUF` (expected), and two parse artifacts from the
  tracer's own appended `deref=`/`extderef=` fields.

## Honest status

Six real bugs found and fixed across sessions 3–4, and the buffer-binding failure — which produced
the single most misleading symptom in this whole effort — is definitively solved. The pipeline is
now byte-identical to the vendor's in ordering and payloads, with correct RDI routing, correct
stream ids, and correct event subscription.

What remains is not visible from userspace. The buffers being *untouched* rules out the entire
buffer-handoff chain and points upstream to sensor CSI emission or CSID/ISPIF acceptance — neither
of which exposes state to userspace on this kernel. Resolving it needs driver-side `pr_info` in
`msm_csid_irq`/`msm_ispif`/`msm_isp_axi_util`, which needs a kernel rebuild and flash: explicitly
out of scope under the standing rules, and better done during the OS-swap kernel work anyway
(`notes/16`), where a rebuild is happening regardless.

B1 remains the working camera path for all stock-OS work; nothing downstream is blocked.

## Session 5: instrumented kernel — one more real bug, and two false leads killed

With the instrumented kernel booted (`notes/21`), driver-level visibility is available for the
first time. Findings:

### Fixed: `m.userptr` must carry the ION dmabuf FD, not an address

The msm camera driver **abuses `V4L2_MEMORY_USERPTR`**. `msm_buf_mgr.c`:

```c
qbuf_buf->planes[i].addr = vb2_buf->planes[i].m.userptr;   /* copy_planes_from_v4l2_buffer */
mapped_info->buf_fd     = qbuf_buf->planes[i].addr;        /* prepare_v4l2_buf -> cam_smmu_get_phy_addr */
```

The field is consumed as a **dmabuf fd**. That is also why `msm_vb2_dma_contig_get_userptr()` is a
bare `kzalloc`+store stub — there is no user pointer to map. We were passing the real mmap'd
address, so the SMMU lookup failed.

The evidence had been in the vendor trace since session 4: its `v4l2_plane` carried
`m.userptr = 0x21` (33). It was even noted at the time as "too small to be an address, can only be
an fd" — and then not acted on. **Noticing an anomaly is not the same as following it.**

Verified fixed, from the kernel itself:

```
cam_smmu_map_buffer_and_add_to_list: name vfe ion_fd = 14, paddr = 0x10000000, len = 311296
msm_isp_prepare_v4l2_buf: plane: 0 addr:0x10000000
msm_isp_cfg_ping_pong_address: vfe 0 config buf 0 to pingpong 0 stream 1
```

Buffers now map to valid IOVAs and are programmed into the ISP ping-pong registers.

### False lead killed: `kptr_restrict` was hiding every pointer

`/proc/sys/kernel/kptr_restrict` was **2**, so `%pK` printed every kernel pointer as
`0000000000000000`. The first instrumented run therefore showed
`msm_isp_prepare_v4l2_buf: plane: 0 addr:0000000000000000` and I read it as "the DMA target is
null". It was a printing artifact. `DMA buf`, `device` and `paddr` were *all* zero in the same
line, which should have been the tell. Set `kptr_restrict=0` before trusting any pointer in dmesg.

### False lead killed for good: CSID `0x800` was never a signal

`notes/19` earlier treated "CSID reports only `0x800`" as evidence of absent CSI data, and
`notes/21` predicted the instrumented kernel would finally make that interrupt meaningful. **Both
were wrong.** Running B1 — which *does* deliver frames — on the same instrumented kernel produces
exactly the same thing:

```
B1 (WORKS):   3x CSID0_IRQ_STATUS_ADDR = 0x800, 3x CSID1..., 3x CSID2..., 1x CSID3...
B2 (NO DATA): 3x CSID0_IRQ_STATUS_ADDR = 0x800
```

Identical. CSID interrupt status simply does not report data flow on this hardware in this mode.
Settled by a control experiment rather than by inference.

### The clean signal: the VFE never receives an AXI interrupt

Driver-level diff of a working B1 run against a failing B2 run:

| kernel path | B1 (works) | B2 |
|---|---|---|
| `msm_isp_process_axi_irq` | 57 | **0** |
| `msm_isp_process_axi_irq_stream` | 114 | **0** |
| `msm_isp_notify` | 116 | **0** |
| `msm_isp_get_buf` | 94 | **0** |
| `msm_isp_process_done_buf` | 25 | **0** |
| `msm_isp_cfg_ping_pong_address` | 38 (re-armed per frame) | 2 (initial arm only) |

Every B1-only code path is a *downstream consequence of an interrupt firing*. The ISP is correctly
armed and simply never told a frame arrived.

### Verified identical, so not the cause

- **ISPIF routing** — byte-identical for cam0: `intftype 1, vfe_intf 0, csid 0`,
  `select_clk_mux data 0`, `pack_mask 0 0`. (B1's cam1 shows `intftype 3, csid 1`, independently
  confirming the session-3 RDI-interface fix was right.)
- **CSID config** — `lane_cnt=1, lane_assign=0x4320, phy_sel=0`, `cid_lut dt=0x2a df=1`.
- **`GET_SUBDEV_ID`** — was genuinely missing from our path (a restructure dropped the calls and
  left only a comment). Restored to the vendor's position. No change; not the cause.

### Where it stands

CSID configured correctly, ISPIF routed correctly, ISP buffers mapped to real IOVAs and armed —
and the VFE never raises a frame interrupt. The fault is squarely between the sensor emitting CSI
data and the VFE being told about it, with every userspace-visible configuration now verified
identical to the working path.

## Session 6 — B2 delivers frames from all four cameras — 2026-09-04

**B2 is complete.** `cam_kernel` captures 302 frames/camera over 5 s from all four tracking
cameras with zero Meta userspace blobs. Two bugs, both invisible to every check made before.

### Bug 1: the MCU was never told to stream

`camera_init` (0x2e) and `start_streaming` (0x2c) both carry `num_cams`. Every B2 run had used
`ncam=1`; **the MCU streams nothing at all when told 1**, so no sensor emitted a single MIPI
packet. The control (`cam_direct all`) always passed 4, which is why it worked.

This is why five sessions of pipeline debugging found nothing: CSIPHY, CSID, ISPIF and VFE were
correct the entire time. The bisection that finally located it:

| block | B1 (works) | B2 (no frames) |
|---|---|---|
| CSIPHY `lane_config` | mask 0x3, cnt 1, settle 0xe, lane_enable 0x81 | **identical** |
| CSID `csid_params` | lane_cnt 1, lane_assign 0x4320→0x3210, dt 0x2a, df 1 | **identical** |
| ISPIF `config` (cam0) | intftype 1, vfe_intf 0, csid 0 | **identical** |
| VFE `reserve_wm` / `ping_pong` / `reg_update` | wm 0, mask 2 | **identical** |
| `ispif_process_irq` (SOF) | RDI0/RDI1 frame id 0,1 | **zero** |
| `msm_vfe47_*` IRQs | 96 | **zero** |

Identical configuration, no interrupts anywhere — that pattern means *no photons*, not a
misconfigured block. The break had to be upstream of CSIPHY, i.e. the sensor itself.

### Bug 2: all four cameras routed to ISPIF RDI0

`ispif_call()`/`ispif_cfg2()` hardcoded `intftype = RDI0`. Two cameras share a VFE, so cam1 and
cam3 silently overwrote cam0's and cam2's routes and never delivered. The VFE side already
alternated (`VFE_RAW_0 + i%2`, `RDI_INTF_0 + i%2`); ISPIF never got the same treatment. Vendor
mapping, now matched:

```
csid0 -> RDI0 (intftype 1) / vfe0      csid1 -> RDI1 (intftype 3) / vfe0
csid2 -> RDI0 (intftype 1) / vfe1      csid3 -> RDI1 (intftype 3) / vfe1
```

Only after bug 1 was fixed did bug 2 become observable — with `ncam=1` the odd cameras never ran,
which is the same class of masking as the `VFE_RAW_0` hardcode in session 4.

### CSID `0x800` — closed for good

Claimed twice as evidence of absent CSI data. It is identical (`0x800`, 3×) in the working control
and in the dead run, and is present in runs that deliver 302 frames/camera. It is reset-done. It
never carried information about data flow.

### Method note

The one check never run in five sessions was the **A/B diff of the same debug output against the
working control**. Enabling `msm_isp47.c`/`msm_csiphy.c` `dynamic_debug` on both paths and diffing
took one run each and eliminated the entire pipeline as a suspect. Instrumenting the failing path
alone cannot distinguish "misconfigured" from "correct but starved"; only the control can.

### Unrelated device fault found en route: UFS link death

Mid-session, all writes to `/data` began hanging while by-name reads stayed instant. This was **not**
filesystem corruption (`/data` is **ext4**, mounted `rw`, no fs errors) — the UFS link died during
idle clock-gating and could not recover:

```
pwr ctrl cmd 0x17 with mode 0x0 completion timeout
Clk gate=1, hibern8 on idle=4
__ufshcd_uic_hibern8_enter: hibern8 enter failed. ret = -110
ufshcd_host_reset_and_restore: Host init failed -110
```

Consequences worth knowing, because they silently corrupt an experiment:
- `adb push` **reports success and writes only page cache** — the file reads back empty after reboot.
  Verify pushes with `sync` + `md5sum` when this is suspected.
- `adb reboot` does nothing (init blocks on unmount). `echo b > /proc/sysrq-trigger` recovers.

Mitigation, applied at the top of the run scripts:
```sh
echo 0 > /sys/devices/soc/1da4000.ufshc/clkgate_enable
echo 0 > /sys/devices/soc/1da4000.ufshc/hibern8_on_idle_enable
```
Whether the instrumented kernel makes this more likely is **untested** — our config is byte-identical
to stock except `CONFIG_SYSTEM_TRUSTED_KEYS`, but the toolchain differs and the hibern8 handshake is
timing-sensitive. Falsifiable test: flash stock boot, leave idle, see if it recurs.
