# Compositor / display — not yet a component

There is no replacement binary here yet: the plan is to configure Monado (an existing OpenXR
runtime), not to write our own compositor, so there's nothing of ours to give a `components/`
directory to until that integration work starts (`research-notes/18` step 5 task 5, promoted to its
own step 6 once it became clear "Monado as OpenXR runtime" was hiding a large workstream,
`research-notes/18` step 6 preamble).

## Status: characterisation done, integration not started

- **Panel timing**: 71.8209 Hz (not 72), video mode, dual DSI, 13.718 ms bottom-to-top rolling
  scanout with only 0.206 ms vblank — reprojection must track scanout phase, it is not a global
  flash (`research-notes/44`).
- **Distortion**: Meta's per-eye distortion mesh is decoded (`tools/display/decode_distortion_mesh.py`)
  and converted to a Monado-consumable sampler format (`tools/display/mesh_to_monado.py`)
  (`research-notes/42`).
- **Persistence**: low persistence appears to be panel-native — no software path drives it, so a
  replacement stack inherits it for free, nothing to implement (`research-notes/46`).
- **Display clock**: `0x55` (vsync) is on the same IMU clock as everything else
  (`research-notes/47`), which is what timing needs — but *attributing* an injected pose to a
  specific displayed frame (motion-to-photon) is not yet done; see `docs/tracking.md`.

## Why this matters now, not just at the OS swap

`components/tracking`'s own acceptance criterion needs motion-to-photon latency "within 2× of
stock", which can't be evaluated without a characterised reference — and that reference is
perishable: once the OS swap deletes `/vendor`, Meta's compositor is gone and can no longer be
driven with a known pose to measure against (`research-notes/23`, `research-notes/18` step 6).

## When this becomes a component

Once Monado bring-up starts against the assets above (a config, not a binary we're writing), this
file's contents move to a `components/compositor/` (or `components/monado-config/`) doc, and that
directory's build step becomes reproducing the Monado config generation the same way the other
components reproduce their binaries.
