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
