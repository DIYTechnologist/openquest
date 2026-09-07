# Quest 1 Sensor/Camera Tap — Access Paths for an Open Tracker

Date: 2026-08-30
Device: `1PASH9ACHD0215` / `monterey` (msm8998), persistent Magisk root.
Raw: `recon/sensor-tap-2026-08-30/`. Follows [[04-openvr-stack-survey]], [[05-calibration-export]].

Goal: find how to feed the 4 tracking cameras + IMU to an open tracker (Monado/Basalt),
i.e. where the hardware-interface boundary is and what is open vs blob.

## How Meta's stack accesses hardware

`trackingservice` (pid 900) opens **no** `/dev/video` or IMU node directly. Its fds are
`/dev/hwbinder`, `/dev/binder`, `/dev/ion`, and many `/dev/ashmem` + sockets. So it consumes
frames/IMU over **HIDL (hwbinder) + ION/ashmem shared buffers** from the HALs below.

`lshal` shows the tracking hardware is behind a **custom Oculus HIDL HAL** — not the standard
Android camera stack (`media.camera` service does **not exist** on this device):

```
vendor.oculus.hardware.sensors@1.0::ICameraProvider     # the 4 tracking cameras
vendor.oculus.hardware.sensors@1.0::IImu                # IMU
vendor.oculus.hardware.sensors@1.0::IMag                # magnetometer
vendor.oculus.hardware.sensors@1.0::IControllerProvider # controller tracking
android.hardware.sensors@2.0::ISensors                  # standard sensors (minimal here)
```

Both the sensor stream and the tracking cameras are served by **one** daemon:
`/vendor/bin/hw/vendor.oculus.hardware.sensors@1.0-service` (pid 772).

## The sensor frontend: Syncboss (KEY — an open path exists)

The IMU/mag/controllers come from **Syncboss**, an SPI-attached nRF52 coprocessor that
timestamps and streams sensor data. It has a dedicated Linux driver and readable char devices:

| Node | mode | role |
|---|---|---|
| `/dev/syncboss_stream0` | `crw-rw-r--` system:system | **sensor data stream (IMU/mag/controller packets)** |
| `/dev/syncboss_control0` | `crw-rw-r--` | control channel |
| `/dev/syncboss_powerstate0` | `crw-rw-r--` | power state |
| `/dev/syncboss0` | `crw-rw-r--` | main / SWD firmware flash |

- Driver: `/sys/bus/spi/drivers/oculus_syncboss` (bound to `spi12.0`), misc-class char devices.
- MCU firmware blob: `/vendor/firmware/syncboss.bin` (147,920 B) — runs **on the coprocessor**,
  not the SoC; an open stack keeps loading it. The streaming protocol is handled by the
  **kernel driver**, which is part of Meta's GPL-obligated Quest kernel source
  (`drivers/.../syncboss`). *TODO: pull the exact driver source from the kernel drop for this
  build to lock down the packet format — but the on-device evidence (open driver + readable
  stream node) already shows the path is open.*
- Current holder: `sensors@1.0-service` (pid 772) has all three open (likely exclusive). To
  read directly, an open stack stops that HAL and opens `/dev/syncboss_stream0` itself.

**Implication:** IMU + mag + controllers can be driven with **no Meta userspace blob** — open
kernel driver, readable stream node, coprocessor firmware retained as-is. This is the cleanest
possible outcome for the inertial half of tracking.

## The cameras: Qualcomm V4L2 under the Oculus HAL

Standard QC msm camera subsystem (CAMX/CAMSS) V4L2 nodes:

| node | name |
|---|---|
| `/dev/video0` | msm-config |
| `/dev/video1` | msm_jpegdma |
| `/dev/video2` | sde_rotator |
| `/dev/video3` | **msm-sensor** (capture) |
| `/dev/video32,33` | (unnamed — likely tracking-cam output buffers) |
| `/dev/media0..4`, `/dev/v4l-subdev0..13` | media graph + CSIPHY/CSID/IFE/sensor subdevs |

Nodes are `system:camera 0660` (video32/33 are `0666`). There is **no** separate camera
provider binary in `/vendor/bin/hw`; the `sensors@1.0-service` HAL itself implements
`ICameraProvider` and drives the QC pipeline.

Two camera-tap options for an open tracker:

| Option | What | Cost |
|---|---|---|
| **A. Reuse Oculus HAL** | call `vendor.oculus.hardware.sensors@1.0::ICameraProvider` over hwbinder | keeps one blob, but a stable interface; fastest to a working tracker |
| **B. Raw V4L2** | drive `/dev/video*` + media graph directly (CSIPHY→CSID→IFE→sensor) | no blob, but must configure the QC camera pipeline by hand — hard on msm8998 downstream CAMX |

## Recommended tap architecture

```
  open runtime (Monado) + open tracker (Basalt)
        |  IMU/mag                 |  camera frames
        v                          v
  /dev/syncboss_stream0       Option A: ICameraProvider (HIDL, blob)   <-- start here
  (open kernel driver)        Option B: /dev/video* V4L2  (fully open) <-- stretch goal
```

Start with **Syncboss-direct IMU + HAL-provided camera** (replaces `trackingservice`/`vrapi`
while reusing the proven camera pipeline); later migrate cameras to raw V4L2 to drop the last
blob. Calibration for both is already exported/converted ([[05-calibration-export]]).

## Verified facts / next steps

- [x] No standard Android camera service; tracking HW is behind `vendor.oculus.hardware.sensors@1.0`.
- [x] Syncboss driver open (`oculus_syncboss`), stream node readable, MCU fw is `syncboss.bin`.
- [x] Camera = QC V4L2 (`/dev/video3` msm-sensor + media graph).
- [ ] Pull `oculus_syncboss` from the kernel source drop for this build; document the
      `syncboss_stream0` packet format (IMU/mag/controller records + timestamps).
- [ ] Decide camera tap A vs B; if A, dump the `ICameraProvider` HIDL interface methods.
- [ ] Prototype: stop `sensors@1.0-service`, read `/dev/syncboss_stream0`, confirm IMU packets.
      (Note: this halts head tracking until restarted — do it deliberately, not casually.)
