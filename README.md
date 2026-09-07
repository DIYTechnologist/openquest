# Quest 1 — open VR stack on `monterey`

Reverse-engineering and tooling to replace Meta's proprietary VR blobs with an open stack
(OpenVINS/Monado) on a **Quest 1** (`monterey`, Snapdragon 835 / msm8998), on an owned device.
Strategy: replace Meta's services **one at a time on the stock OS** (`research-notes/17`,
`research-notes/18`), so the eventual OS swap is a port of known-working code with a known-good
fallback, not a big-bang rewrite.

> This repo versions **source, notes, and small text/JSON artifacts** only. Large device dumps and
> pulled vendor binaries are `.gitignore`d — see [Building](#building). Some excluded data
> (`backups/`, factory calibration) is **per-unit and sensitive** (serials, keys); do not publish it.

## Layout

- **[`components/`](components/)** — one directory per Meta service being replaced, each
  independently buildable via its own `Makefile`: `camera/`, `controllers/`, `tracking/`, `kernel/`.
  See **[`docs/`](docs/)** for what each replaces, its status, and how to run it.
- **[`tools/`](tools/)** — everything supporting that work which isn't itself a shipped replacement
  binary: reverse-engineering/diagnostic tools (some superseded, kept for reference) and offline
  research scripts (VIO accuracy measurement, calibration conversion, dataset building).
- **[`research-notes/`](research-notes/)** — the chronological research record, read in numeric
  order. Start from `research-notes/52-CHECKPOINT-ate-controllers-display.md` and
  `research-notes/53-room-scale-ate-static-init.md` for the current state; earlier notes are kept
  as-written even where later notes supersede them.
- `recon/`, `exports/`, `devicetree/` — text findings, converted calibration, DT dumps (large
  binaries within are gitignored).

## Status

See [`docs/README.md`](docs/README.md) for the per-component status table. Summary: camera capture
and controller input decoding are done with zero Meta userspace code; tracking (VIO + pose
injection into Meta's own compositor) is running with a measured accuracy of 7.6 cm ATE in-place
and 11.7 cm ATE over a 60.6 m room-scale walk against Meta's own tracker as ground truth; the
kernel build is reproducible and boots; display/compositor characterisation is done but Monado
integration hasn't started; the OS swap hasn't started (gated on the above).

## Building

Each component cross-compiles inside a container — no Android NDK, kernel toolchain, or
OpenCV/Boost/Eigen/OpenVINS needs installing on the host, only `git` and `podman` (or `docker`):

```
make base-images   # one-time: build the shared toolchain images (build/containers/)
make               # build every component
```

See [`docs/README.md`](docs/README.md#building) for building/running a single component, and
`tools/hidl-build/`, `tools/aosp-headers/` for the (unchanged, host-side) regeneration steps that
tooling under `tools/` still needs, documented at the top of each of those directories.

## Scope

Owner-authorized work on one owned Quest 1. No third-party systems, accounts, or content
protection are in scope. Reversible throughout (stock `boot_a` preserved in `backups/`).
