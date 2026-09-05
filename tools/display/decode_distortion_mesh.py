#!/usr/bin/env python3
"""decode_distortion_mesh.py — parse Meta's baked lens-distortion mesh (Quest 1 / monterey).

Source: /system/etc/calibration/distortion-mesh.bin (52,368 B). Located in notes/32 after
establishing that the distortion is *data*, not code — there is not a single distortion symbol in
libvrapi.so, vrapiserver, libossdk.oculus.so or the composer HAL.

Layout (fully decoded, notes/42):

    offset 0x00  u32  magic 0x56347807
    offset 0x08  u32  version-ish 257, then 259
    offset 0x18  u32  grid 32 x 32 (cells; 33 x 33 vertices)
    offset 0x30  u32  2880 1600 1216 1344   panel w,h then per-eye render target w,h
    offset 0x40  f32  47 53 52 42 / 47 53 42 52   FOV half-angles (deg), L/R mirrored
    offset 0x60  ...  2178 vertices, each 3 channels (R,G,B) x 2 floats (x,y)

The part that defeated four earlier readings: the 2178 vertices are **66 rows x 33 columns with the
two eyes interleaved by row** — even rows are one eye, odd rows the other. Every attempt to split
the eyes as two contiguous blocks produces a grid that is monotonic in neither axis. De-interleaved
by row parity, both eyes are 33x33 and 100% monotonic in x along rows and y down columns, and are
exact mirrors of each other.

Three channels per vertex is chromatic-aberration correction: a separate mesh for R, G and B.
Measured offsets from green are small and non-zero (|R-G| ~ 0.006, |B-G| ~ 0.013).

Coordinate space is NOT normalised UVs — the range is about -3.8..3.6, and the extremes correspond
to ~75 degrees of half-angle, consistent with a tangent-space (tan of field angle) parameterisation
given the header's 42-53 degree FOV numbers. Stated as consistent-with, not proven: nothing here
depends on it, and a mesh consumer only needs the grid.

Usage: decode_distortion_mesh.py <distortion-mesh.bin> [--npz out.npz]
"""
import struct
import sys

import numpy as np

HDR = 96
N = 33                      # vertices per side
NV = 2178                   # 66 rows x 33 cols, both eyes interleaved
ROWS = 66


def decode(path):
    d = open(path, 'rb').read()
    magic, = struct.unpack('<I', d[:4])
    if magic != 0x56347807:
        raise SystemExit(f'bad magic {magic:#x} (expected 0x56347807)')
    v1, v2 = struct.unpack('<I', d[8:12])[0], struct.unpack('<I', d[16:20])[0]
    gw, gh = struct.unpack('<2I', d[0x18:0x20])
    panel_w, panel_h, eye_w, eye_h = struct.unpack('<4I', d[0x30:0x40])
    fov = struct.unpack('<8f', d[0x40:0x60])

    f = np.frombuffer(d[HDR:], dtype='<f4')
    expect = NV * 3 * 2
    if f.size != expect:
        raise SystemExit(f'payload {f.size} floats, expected {expect}')
    grid = f.reshape(ROWS, N, 3, 2)
    eyes = (grid[0::2], grid[1::2])          # row parity de-interleaves the eyes

    meta = dict(version=(v1, v2), grid=(gw, gh), panel=(panel_w, panel_h),
                eye_target=(eye_w, eye_h), fov_deg=fov)
    return meta, eyes


def main():
    if len(sys.argv) < 2:
        raise SystemExit(__doc__)
    meta, eyes = decode(sys.argv[1])
    print(f"version      {meta['version']}")
    print(f"grid         {meta['grid'][0]} x {meta['grid'][1]} cells -> {N} x {N} vertices")
    print(f"panel        {meta['panel'][0]} x {meta['panel'][1]}")
    print(f"eye target   {meta['eye_target'][0]} x {meta['eye_target'][1]}")
    print(f"FOV half-deg L={meta['fov_deg'][:4]} R={meta['fov_deg'][4:]}")
    for i, e in enumerate(eyes):
        g = e[:, :, 1, :]                    # green
        xr = np.mean([np.all(np.diff(g[r, :, 0]) > 0) for r in range(N)])
        yc = np.mean([np.all(np.diff(g[:, c, 1]) > 0) for c in range(N)])
        print(f"eye{i}: x monotonic along rows {xr*100:.0f}%  y monotonic down cols {yc*100:.0f}%  "
              f"x {g[..., 0].min():.3f}..{g[..., 0].max():.3f}  y {g[..., 1].min():.3f}..{g[..., 1].max():.3f}")
        dr = np.abs(e[:, :, 0, :] - e[:, :, 1, :]).mean()
        db = np.abs(e[:, :, 2, :] - e[:, :, 1, :]).mean()
        print(f"      chromatic offset from green: |R-G|={dr:.5f} |B-G|={db:.5f}")
    if '--npz' in sys.argv:
        out = sys.argv[sys.argv.index('--npz') + 1]
        np.savez(out, eye0=eyes[0], eye1=eyes[1], **{k: np.array(v) for k, v in meta.items()})
        print(f"wrote {out}")


if __name__ == '__main__':
    main()
