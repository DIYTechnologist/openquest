# Distortion mesh fully decoded — 6A task 3's source is in hand — 2026-09-05

`notes/32` located Meta's lens distortion as **data, not code** (no distortion symbol exists in
`libvrapi.so`, `vrapiserver`, `libossdk.oculus.so` or the composer HAL) and read the header. The
vertex block resisted four readings. It is now fully decoded, with a parser in
`tools/display/decode_distortion_mesh.py`.

## Layout

```
0x00  u32   magic 0x56347807
0x08  u32   257          0x18  u32  32 x 32 cells -> 33 x 33 vertices
0x10  u32   259          0x30  u32  2880 1600 1216 1344   panel w,h then per-eye target w,h
                         0x40  f32  47 53 52 42 / 47 53 42 52   FOV half-angles (deg), L/R mirrored
0x60  payload: 2178 vertices x 3 channels (R,G,B) x 2 floats (x,y)  = 13,068 floats
      total 96 + 52,272 = 52,368 B  (exact)
```

**The part that defeated every earlier attempt:** the 2178 vertices are **66 rows x 33 columns with
the two eyes interleaved by row** — even rows one eye, odd rows the other. Every reading that split
the eyes as two contiguous blocks produced a grid monotonic in neither axis, which is why
`notes/32` overstated the layout as "byte-exact confirmed" when only the *size* was confirmed.

De-interleaved by row parity:

```
eye0  33x33   x monotonic along rows 100%   y monotonic down cols 100%
eye1  33x33   x monotonic along rows 100%   y monotonic down cols 100%
eye0 x -3.441..2.286   eye1 x -2.286..3.441   both y -3.760..2.584
```

100% in both axes for both eyes, and the two are exact mirrors (eye0 TL = -eye1 TR). That is four
independent consistency checks passing at once, against a hypothesis space where every other
ordering failed — which is what makes this a decode rather than another guess.

The tell, in hindsight: `y` values appeared **duplicated in pairs** down a column. Two rows sharing
a `y` is exactly what row-interleaved eyes look like.

## Chromatic aberration is corrected per channel

Three meshes per vertex, one each for R, G, B, with small consistent offsets from green:

```
|R-G| = 0.00576      |B-G| = 0.01294      (identical for both eyes)
```

So Meta corrects lateral chromatic aberration in the distortion mesh itself. Any replacement
compositor must do the same or colour-fringe at the edges.

## Coordinate space

Not normalised UVs — the range is about -3.8..3.6. The extremes correspond to ~75 degrees of
half-angle, which is consistent with a tangent-space (tan of field angle) parameterisation given the
header's 42-53 degree FOV figures. Recorded as *consistent with*, not proven: nothing downstream
depends on it, because a mesh consumer needs the grid, not its parameterisation.

## What this closes

6A task 3 was "validate we can reproduce Meta's lens distortion from calibration". The answer is
better than the task assumed: there is nothing to fit. Meta ships the mesh, Monado consumes a
distortion mesh, so the task collapses to a **format conversion** — and the source format is now
parsed, validated and exported.

Remaining for a stated pixel error: render a test pattern through this mesh and compare against
what the stock compositor produces. That still needs the stock compositor alive, so it stays on the
perishable list.

## 6A status

| criterion | state |
|---|---|
| panel timing documented | **done** — superseded by `notes/44`: 71.819 Hz measured (sd 3.4 us), video mode, dual DSI, 13.718 ms bottom-to-top scanout, 0.206 ms vblank |
| distortion reproduced to a stated pixel error | **decoded, parsed and converted** (`mesh_to_monado.py`); error figure still needs a comparison render |
| motion-to-photon measured | not started; `SWAP_TIMING_*` gives the model to check against |

## Converter: `tools/display/mesh_to_monado.py` (added 2026-09-05)

Emits a C header with the grid baked in plus a bilinear sampler whose body matches Monado's
`u_distortion_mesh` callback shape (normalised u,v over the render target -> one UV per colour
channel), so a driver can call it directly. Compiles clean and passes a bounds/corner test.

Two checks pass that were not designed for, and both are physical rather than structural:

```
eye0 (0.5,0.5) -> G(0.5874, 0.5816)     eye1 (0.5,0.5) -> G(0.4126, 0.5816)
                  0.5874 + 0.4126 = 1.0000 exactly -> the eyes are exact mirrors

chromatic offset |B-G|:  centre 0.00000   edge 0.00097
```

Chromatic offset is **zero at the centre and grows toward the edges** — which is exactly how lateral
chromatic aberration behaves, and is not something the normalisation could have manufactured. The
green channel sets the extent so R and B keep their *relative* offsets rather than each being
stretched to fill [0,1], which would have destroyed the correction.

**Metrically provisional.** Normalisation uses the mesh's own min/max extent, which is right if the
grid uniformly samples the render target (its 100 % monotonicity in both axes supports that) and
wrong if the sampling is non-uniform in a way not yet characterised. The topology and the chromatic
offsets are certainly correct; the absolute mapping to field angles still needs the comparison
render against the stock compositor — which is the remaining half of 6A task 3 and stays on the
perishable list.
