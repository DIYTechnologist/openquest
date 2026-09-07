# Matching `k` to the calibration: pairs resolved, within-pair order not — 2026-09-05

`notes/38` established that `k` (ctor position within an id's run of four) is a stable, distinct
camera index. It does not follow that `k == n` in `camera_calibration_v2.json`, and intrinsics and
extrinsics are per-camera, so the assignment has to be established before any metric dataset.

## The calibrated rig has one very distinctive feature

From `DeviceFromCamera` in the factory calibration:

```
cal0  pos (-0.056, -0.040, -0.070)  axis (-0.17, -0.64, -0.75)   bottom-left,  looking down
cal1  pos (-0.074, +0.032, -0.070)  axis (-0.66, +0.35, -0.67)   top-left,     looking left/up
cal2  pos (+0.056, -0.040, -0.070)  axis (+0.17, -0.64, -0.75)   bottom-right, looking down
cal3  pos (+0.074, +0.032, -0.070)  axis (+0.65, +0.36, -0.67)   top-right,    looking right/up

angular separation between optical axes:
  cal0-cal2  19.6 deg   <- the ONLY highly-overlapping pair
  cal0-cal1  67.0        cal2-cal3  67.4
  cal0-cal3  80.6        cal1-cal2  80.4        cal1-cal3  81.7
```

Exactly one pair sits at ~20 degrees; every other pair is 67-82 degrees apart. That is a strong,
motion-independent signature.

## Result: the pairs are matched

Pixel distance between the four identified cameras, from one simultaneous 4-frame dump:

```
k0-k2  0.70   <- the only clearly-overlapping pair
k0-k3  0.85     k1-k3  0.88     k2-k3  0.88     k1-k2  0.89     k0-k1  0.95
```

The structure matches the calibration's: one distinctly-overlapping pair, the rest bunched. So

```
{k0, k2}  <->  {cal0, cal2}     (the down-looking pair, 11.2 cm baseline)
{k1, k3}  <->  {cal1, cal3}     (the outward-up pair,  14.8 cm baseline)
```

That `k0,k2` map to `cal0,cal2` and `k1,k3` to `cal1,cal3` — index-for-index rather than crossed — is
consistent with `k` simply being the calibration order, but the pairing evidence alone does not
prove it.

## Not resolved: which member of each pair is which

Attempted by disparity direction, and the result should not be trusted:

```
k0 vs k2: best horizontal shift  -44 px, residual 0.510
k1 vs k3: best horizontal shift +192 px, residual 1.060   <- at the search limit
```

The `k1/k3` estimate is meaningless — 82 degrees apart, essentially no overlap, and the optimum ran
to the edge of the search range. The `k0/k2` estimate is better but still weak: a pure horizontal
translation is the wrong model for two fisheye views separated by 20 degrees of rotation, so the sign
may be reporting rotation rather than baseline.

Two binary ambiguities therefore remain (which of `k0`/`k2` is `cal0`; which of `k1`/`k3` is
`cal1`) — four candidate assignments. Getting this wrong flips the stereo baseline sign and would
break triangulation silently, so it must be settled properly rather than assumed.

## How to settle it

`tools/vio/epipolar_check.py` already exists and is the right instrument. Undistort a stereo pair
with the `Fisheye62` parameters under each candidate assignment and measure epipolar error: the
correct assignment gives low residuals, a swapped one gives large, systematic ones. Purely offline,
needs no device, and the frames for it are already captured.

Worth noting the whole question is cheap to check and expensive to get wrong, which is the same
shape as the four inference failures in `notes/33`-`notes/38` — so it gets measured, not reasoned
about.
