# Motion-to-photon Phase 0: both software framebuffer-capture paths are blocked — 2026-09-09

`research-notes/47` left one open half of the motion-to-photon measurement: timing is solved (the
nRF clock unifies IMU/camera/vsync), but attributing an injected pose *change* to the specific
*displayed frame* it first appears in was never attempted. The plan (this session) was to read
`/dev/graphics/fb0` directly, on the strength of `research-notes/32`'s finding that
`display.conf` ships an explicit `SWAP_TIMING_FRONT_BUFFER` table — evidence the stock compositor
is front-buffer/direct-mode. Tested directly. **Both natural software paths are dead ends,
independently, for different reasons.** No new device capture beyond this feasibility probe;
nothing was stopped or reconfigured, only read.

## Path 1: `/dev/graphics/fb0` — never written by the real compositor

`/dev/graphics/fb0` exists, is root-readable, `power=active`, `virtual_size=2880,3200` (double-
buffered: panel is 2880x1600, `research-notes/32`/`44`), `stride=11520` (32bpp).

- A plain `read()`/`dd` on the node returns `ENODEV` immediately — standard for fbdev drivers that
  implement `.fb_mmap` but not `.fb_read` (real consumers use mmap).
- `mmap(PROT_READ, MAP_SHARED)` of the full 36 MB virtual buffer **succeeds**, but every single byte
  in it is zero — checked across the *entire* buffer in 100-row bands, not just one sampled strip,
  and confirmed again with a definitely-bright UI element forced on screen (`MtpAlertActivity`,
  the "USB connected" dialog) to rule out "we just sampled a dark frame." Still all zero.
- There is no `/dev/dri/` on this device and only one `fb*` node — so this isn't a case of the real
  scanout living on a different, DRM-based device instead; there's nowhere else fbdev-shaped to
  look.

**Conclusion**: this Snapdragon 835-era MSM display stack composites through the classic Qualcomm
overlay ioctl path (`MSMFB_OVERLAY_*`), not through `fb0`'s pan-flip memory. The `fb0` node survives
only for sysfs (panel info, `vsync_event`, backlight, `dynamic_fps`) and mmap doesn't error because
the driver still answers it — it just backs the mapping with pages that are never the real front
buffer. This matches this project's own established pattern (`research-notes/33`'s "locked VA, not
the real one" lesson for camera ImageBuffers) — a device answering an API call successfully is not
proof the API call reaches real data.

## Path 2: `screencap` — explicitly refused, by design

Android's own `screencap` (which goes through SurfaceFlinger, not fbdev, and normally works
regardless of the underlying HWC composition method) was tried as the fallback. It also fails,
but for an entirely different, more deliberate reason:

```
SurfaceFlinger: FB is protected: PERMISSION_DENIED
SurfaceFlinger: captureScreen failed to readInt32: -1
```

This is content protection: the VR display's buffers are flagged protected (likely
`GRALLOC_USAGE_PROTECTED`), and SurfaceFlinger refuses to hand back pixel data for a protected
display **even to root**. Unlike Path 1 (an accident of which composition path is active), this is
a deliberate policy enforced at the SurfaceFlinger layer specifically to prevent exactly the kind of
capture this measurement needs. No quick property toggles it (`getprop | grep -i protect` — nothing
relevant). Working around it would mean patching SurfaceFlinger itself or finding the specific
vendor flag that sets the protected buffer usage bit — a materially different (and much larger,
riskier) undertaking than a feasibility probe, in the same class of dead end this project has
already ruled off-limits once before (`libtrackingengines.so`, `research-notes/01`).

## What this means for motion-to-photon

**No software-only path is currently known for the photon-attribution half of this measurement.**
Both obvious routes were checked directly rather than assumed, per this project's own standing
verification bar, and both are real, structural blocks, not configuration issues to tune around.

The remaining honest option is external hardware: a photodiode/phototransistor against the lens,
timestamped on the same nRF clock this project already has losslessly available (the `0x55`
vsync/IMU/camera clock, `research-notes/47`) via a simple ADC or GPIO edge sampled by the same
syncboss-adjacent tooling. That is a hardware project, not a software one, and is being recorded as
the next real requirement rather than attempted blind here.

## Housekeeping

New: `tools/motion_to_photon/fb_probe.c` + `build.sh` — the feasibility probe, kept for the record
(it's what produced the all-zero result above) even though the approach it tests is a dead end.
Nothing on-device was stopped, reconfigured, or left in a different state; only reads and one
already-established-safe `am broadcast prox_close` to wake the display for the test. Temp files
(`shot1.png`, `fb_snap.raw`) cleaned up after.
