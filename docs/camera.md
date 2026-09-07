# `components/camera`

Drives all four Quest 1 cameras and reads the head IMU directly against the published msm8998
camera_v2 kernel ABI, with **zero Meta userspace code**. Replaces `libqcameraoculushal.so`,
`libqcameradriver.so`, `libsyncboss.so` (camera/MCU control half), and `cameramuxmodeservice`.

## Status

Done — 5/5 acceptance criteria from `research-notes/18` step 1:
`grep -ci oculus /proc/self/maps` = 0, ≥99% frame delivery (measured 99.43%), FSIN sync preserved
(0.0 µs within a group, 99.86% exposure-phase agreement across groups), self-consistency parity with
the Meta-driven path, and a dataset built from its output drives OpenVINS to a bounded trajectory.
See `research-notes/22`.

## What it does

`cam_kernel` is one binary, one process:

1. Powers and configures all four OV7251 sensors via the same in-kernel `msm_sensor_init` subdev
   probe path the vendor stack uses — no sensor register tables needed (`research-notes/11`).
2. Drives the CSIPHY → CSID → ISPIF → ISP pipeline directly via the published ioctl set
   (`VIDIOC_MSM_CSIPHY_IO_CFG`, `VIDIOC_MSM_CSID_IO_CFG`, `VIDIOC_MSM_ISPIF_CFG{,_EXT}`,
   `VIDIOC_MSM_ISP_*`, plus stock `S_FMT`/`S_PARM`/`REQBUFS`/`QBUF`/`DQBUF`/`STREAMON`), reimplemented
   from a reference trace (`research-notes/19`) rather than guessed from headers alone.
3. In its own thread, reads `/dev/syncboss_stream0` and decodes IMU (`0x50`, 36 bytes, ~994 Hz) and
   camera exposure stamps (`0xe0`) on the MCU's own clock, and drives the MCU camera enable/release
   over `/dev/syncboss0` directly (raw type 40/41 writes) — `libsyncboss.so` is never loaded.
4. Emits one interleaved binary record stream (frames + IMU, `'C'`/`'I'` tagged, both device and
   host timestamps on each record) either to files (dataset mode) or to stdout (stream mode, for
   `components/tracking`).

## Build & run

```
make -C components/camera        # -> components/camera/cam_kernel (aarch64)
adb push components/camera/cam_kernel /data/local/tmp/
adb shell /data/local/tmp/cam_kernel 4 <secs> 3000 160 02 -   # stream mode -> stdout
```

Needs `work/oculus-kernel` (Meta's published GPLv2 kernel source, `research-notes/21`) present in
the checkout — mounted into the build container, not baked into the image.

## Known limits

- `cam_direct` (`tools/cam_direct/`) is kept as the earlier B1 reference (drives the MCU raw but
  still goes through the vendor HAL for pipeline setup) — useful to diff against, not a dependency
  of this component.
- Byte-for-byte parity against the vendor path is unmeasurable in absolute terms (the vendor path
  isn't even self-consistent frame-to-frame beyond 6.4-12.0 LSB); this component matches its own
  self-consistency to within 0.03 LSB instead (`research-notes/22`).
