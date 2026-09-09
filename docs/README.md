# Documentation index

This is the **final-state** description of the project: what each replacement component does and
how to use it today. For the chronological record of how each was reverse-engineered — including
dead ends and corrected mistakes — see [`research-notes/`](../research-notes/), read in numeric
order starting from `research-notes/52-CHECKPOINT-ate-controllers-display.md` and
`research-notes/53-room-scale-ate-static-init.md` for the current state.

| Component | Replaces | Status | Doc |
|---|---|---|---|
| [`components/camera/`](../components/camera/) | `libqcameraoculushal.so`, `libqcameradriver.so`, `libsyncboss.so`, `cameramuxmodeservice` | done | [camera.md](camera.md) |
| [`components/controllers/`](../components/controllers/) | controller half of `vendor.oculus.hardware.sensors@1.0` | input done, 6DoF pose location open | [controllers.md](controllers.md) |
| [`components/tracking/`](../components/tracking/) | `oculus.internal.tracking.ITrackingService` | core done, motion-to-photon latency open | [tracking.md](tracking.md) |
| [`components/kernel/`](../components/kernel/) | stock `boot_a` kernel | instrumented build working | [kernel.md](kernel.md) |
| [`components/os/`](../components/os/) | the stock OS itself (step 5, the OS swap) | just started: device tree drafted, untested, LineageOS source sync in progress | [os.md](os.md) |
| *(not yet a component)* | display/compositor | characterisation done, no replacement binary yet | [compositor.md](compositor.md) |

`tools/` holds everything that isn't a shipped replacement binary: diagnostic/reverse-engineering
tooling (some superseded, kept for reference) and offline research scripts (VIO accuracy
measurement, calibration conversion, dataset building) that support the components above without
being part of what runs on the device.

## Building

Each component is independently buildable and cross-compiles inside a container — no Android NDK,
kernel toolchain, or OpenCV/Boost/Eigen/OpenVINS needs to be installed on the host, only `git` and
`podman` (or `docker`).

```
make base-images         # one-time: builds the shared toolchain images
make                      # builds every component
make -C components/camera # or just one, from the repo root
cd components/tracking && make   # or from inside the component's own directory
```

See `build/containers/` for the toolchain image definitions and `build/mk/container.mk` for how a
component's `Makefile` transparently re-runs itself inside the right image.
