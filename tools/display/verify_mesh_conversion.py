#!/usr/bin/env python3
"""verify_mesh_conversion.py — quantify mesh_to_monado.py's own conversion fidelity, in pixels.

research-notes/42 left "reproduced to a stated pixel error" open, framed as needing a comparison
render against the live stock compositor. research-notes/62 found that comparison is now a
confirmed software dead end (fb0 unwritten by the real compositor, screencap refused as a protected
display) -- the same wall as motion-to-photon. No independent optics/lens calibration source exists
in this project either (research-notes/05's Fisheye62 model is the tracking CAMERAS, unrelated to
the headset's eye optics). So there is no ground truth available, on-device or off, to check the
mesh's ABSOLUTE metric correctness against.

What CAN be checked without a device, and is a real, separate question from that: does
mesh_to_monado.py's generated C sampler (quest1_mesh_sample) actually reproduce the source mesh data
it was built from, and how much does bilinear interpolation between the 33x33 control points differ
from a smoother fit through the same points? Both bound the CONVERTER's own fidelity, not agreement
with Meta's real optics -- a materially different, weaker claim, stated honestly as such.

Two checks, both against the REAL generated C artifact (compiled and run via mesh_verify.c, not a
Python reimplementation of the sampler that could silently diverge from the shipped code):

1. Exact-vertex round-trip: at every one of the 33x33 grid vertices, the C sampler must reproduce
   the source value that vertex was built from. Should be ~0 (float32 rounding only); a real
   discrepancy would mean a genuine bug in mesh_to_monado.py's normalisation or header emission.
2. Off-grid interpolation sensitivity: at cell midpoints, compare the C sampler's bilinear result
   against a separable Catmull-Rom bicubic fit through the SAME 33x33 control points (no scipy
   dependency; implemented here). This bounds how much the CHOICE of interpolation scheme moves the
   answer between control points -- a legitimate, honest number in the absence of a finer ground
   truth, not a measurement of real-world accuracy.

Usage: verify_mesh_conversion.py <distortion-mesh.bin>
"""
import json
import os
import struct
import subprocess
import sys
import tempfile

import numpy as np

sys.path.insert(0, os.path.dirname(__file__))
import decode_distortion_mesh as ddm
import mesh_to_monado as m2m

HERE = os.path.dirname(os.path.abspath(__file__))


def catmull_rom_1d(p0, p1, p2, p3, t):
    # Standard uniform Catmull-Rom basis, vectorised over trailing dims.
    return 0.5 * ((2 * p1) + (-p0 + p2) * t + (2 * p0 - 5 * p1 + 4 * p2 - p3) * t ** 2
                  + (-p0 + 3 * p1 - 3 * p2 + p3) * t ** 3)


def bicubic_sample(grid, u, v):
    """grid: (N,N,...) control points, u/v in [0,1] over the grid. Separable Catmull-Rom."""
    n = grid.shape[0]
    fx, fy = u * (n - 1), v * (n - 1)
    x0, y0 = int(np.floor(fx)), int(np.floor(fy))
    tx, ty = fx - x0, fy - y0

    def clamp(i):
        return min(max(i, 0), n - 1)

    rows = []
    for dy in (-1, 0, 1, 2):
        cols = [grid[clamp(y0 + dy), clamp(x0 + dx)] for dx in (-1, 0, 1, 2)]
        rows.append(catmull_rom_1d(cols[0], cols[1], cols[2], cols[3], tx))
    return catmull_rom_1d(rows[0], rows[1], rows[2], rows[3], ty)


def main():
    if len(sys.argv) < 2:
        raise SystemExit(__doc__)
    mesh_path = sys.argv[1]

    eyes, meta = m2m.decode(mesh_path)
    eye_w, eye_h = meta['eye']
    norm = [m2m.normalise(e)[0] for e in eyes]   # [view][row][col][ch][u,v], in [0,1]

    with tempfile.TemporaryDirectory() as td:
        hdr = os.path.join(td, 'quest1_distortion_mesh.h')
        # Reuse mesh_to_monado's own header writer by calling its module as a script would --
        # simplest way to guarantee we test the EXACT bytes it emits, not a re-derivation of them.
        subprocess.run([sys.executable, os.path.join(HERE, 'mesh_to_monado.py'), mesh_path,
                         '-o', hdr], check=True, capture_output=True)
        exe = os.path.join(td, 'mesh_verify')
        subprocess.run(['gcc', '-O2', '-I', td, '-o', exe,
                         os.path.join(HERE, 'mesh_verify.c'), '-lm'], check=True)
        out = subprocess.run([exe, '129'], check=True, capture_output=True, text=True).stdout

    rows = [line.split(',') for line in out.splitlines() if not line.startswith('#')]
    data = np.array(rows, dtype=float)   # view,u,v,ru,rv,gu,gv,bu,bv
    N = 33

    print("## Check 1: exact-vertex round-trip (converter fidelity, not optics)")
    worst_px = 0.0
    for view in range(2):
        vd = data[data[:, 0] == view]
        for r in range(N):
            for c in range(N):
                u, v = c / (N - 1), r / (N - 1)
                match = vd[(np.abs(vd[:, 1] - u) < 1e-4) & (np.abs(vd[:, 2] - v) < 1e-4)]
                if len(match) == 0:
                    continue
                sampled_g = match[0, 5:7]          # gu, gv
                truth_g = norm[view][r, c, 1, :]   # green channel, stored value
                d = sampled_g - truth_g
                px = abs(d[0]) * eye_w
                py = abs(d[1]) * eye_h
                worst_px = max(worst_px, px, py)
    print(f"  worst vertex deviation: {worst_px:.6f} px (over a {eye_w}x{eye_h} eye target) "
          f"-- {'OK, float32 rounding only' if worst_px < 0.01 else 'REAL DISCREPANCY, investigate'}")

    print("\n## Check 2: bilinear vs. bicubic at cell midpoints (interpolation-scheme sensitivity)")
    rng = np.random.default_rng(0)
    max_dev_px = 0.0
    devs = []
    for view in range(2):
        g = norm[view][:, :, 1, :]   # green channel control points, (N,N,2)
        vd = data[data[:, 0] == view]
        for _ in range(500):
            r = rng.integers(0, N - 1)
            c = rng.integers(0, N - 1)
            u = (c + 0.5) / (N - 1)
            v = (r + 0.5) / (N - 1)
            match = vd[(np.abs(vd[:, 1] - u) < 1e-4) & (np.abs(vd[:, 2] - v) < 1e-4)]
            if len(match) == 0:
                continue
            bilinear = match[0, 5:7]
            bicubic = bicubic_sample(g, u, v)
            d = bilinear - bicubic
            px, py = abs(d[0]) * eye_w, abs(d[1]) * eye_h
            devs.append(max(px, py))
    devs = np.array(devs)
    print(f"  {len(devs)} midpoint samples: median {np.median(devs):.3f} px, "
          f"95th pct {np.percentile(devs, 95):.3f} px, max {devs.max():.3f} px")
    print("  (this bounds how much interpolation SCHEME choice moves the answer between the 33x33")
    print("   control points -- it is not a measurement against real optics; no ground truth finer")
    print("   than this mesh exists offline, and live photon comparison is blocked, research-notes/62)")


if __name__ == '__main__':
    main()
