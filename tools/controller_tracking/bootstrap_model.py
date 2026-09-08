#!/usr/bin/env python3
"""bootstrap_model.py — build an initial LED constellation model by stereo-triangulating one
controller-tracking-exposure frame instant.

No LED 3D geometry exists anywhere for this project and none is extractable (research-notes/01
flags libtrackingengines.so as a deliberate dead end) -- research-notes/55's plan is to bootstrap a
model ourselves from stereo triangulation instead of looking one up. This is that step: pick the
frame instant (from a tools/cam_tap capture, e.g. exports/controller-constellation-2026-09-07/cap9)
with the richest simultaneous two-camera blob view, stereo-match blobs by epipolar consistency
(everything reused from tools/vio/rectify_pair.py's and epipolar_check.py's KB4 math -- not
reimplemented), and triangulate. The resulting 3D point set, expressed in the IMU frame AT THAT
INSTANT, becomes the canonical model -- there is no independent "controller body frame" to anchor
to, so this instant's observed geometry defines one by convention, same as any first calibration
frame would.

Usage: bootstrap_model.py <cap_dir> <basalt_calibration.json> <camA> <camB> <out_model.json>
                          [--expo-us 38] [--max-lag-ns 100000]
"""
import argparse
import json
import math
import sys

import numpy as np

sys.path.insert(0, __file__.rsplit('/', 1)[0])
from blob_detect import find_blobs, find_static_positions, filter_static

W, H = 640, 480  # after dropping the metadata row


def quat_to_R(d):
    q = np.array([d['qx'], d['qy'], d['qz'], d['qw']], float)
    q /= np.linalg.norm(q)
    x, y, z, w = q
    return np.array([
        [1-2*(y*y+z*z), 2*(x*y-z*w),   2*(x*z+y*w)],
        [2*(x*y+z*w),   1-2*(x*x+z*z), 2*(y*z-x*w)],
        [2*(x*z-y*w),   2*(y*z+x*w),   1-2*(x*x+y*y)]])


def se3(d):
    T = np.eye(4)
    T[:3, :3] = quat_to_R(d)
    T[:3, 3] = [d['px'], d['py'], d['pz']]
    return T


def kb4_unproject(pts, intr):
    """Pixel coords (N,2) -> unit bearing vectors (N,3) in that camera's own frame."""
    fx, fy, cx, cy = intr['fx'], intr['fy'], intr['cx'], intr['cy']
    k = [intr['k1'], intr['k2'], intr['k3'], intr['k4']]
    out = []
    for u, v in pts:
        x, y = (u - cx) / fx, (v - cy) / fy
        rd = math.hypot(x, y)
        if rd < 1e-9:
            out.append([0.0, 0.0, 1.0]); continue
        th = rd
        for _ in range(20):  # Newton solve rd = th*(1 + k1 th^2 + k2 th^4 + k3 th^6 + k4 th^8)
            th2 = th*th
            f = th*(1 + k[0]*th2 + k[1]*th2**2 + k[2]*th2**3 + k[3]*th2**4) - rd
            d_ = 1 + 3*k[0]*th2 + 5*k[1]*th2**2 + 7*k[2]*th2**3 + 9*k[3]*th2**4
            th -= f / d_
        s = math.sin(th)
        out.append([s*x/rd, s*y/rd, math.cos(th)])
    return np.array(out)


def load_index(idx_path):
    rows = []
    for line in open(idx_path):
        p = line.split()
        rows.append(dict(fno=int(p[0]), cam=int(p[1]), ts=int(p[2]), mono=int(p[3]),
                          off=int(p[4]), size=int(p[5]), expo=float(p[8]), gain=float(p[9])))
    return rows


def read_frame(blob, off, size):
    blob.seek(off)
    buf = blob.read(size)
    if len(buf) != size:
        return None
    im = np.frombuffer(buf, dtype=np.uint8).reshape(H + 1, W)
    return im[1:]  # drop metadata row 0


def triangulate(o1, d1, o2, d2):
    """Closest point between two rays (origin o, unit direction d). Returns the midpoint and the
    residual gap (a triangulation-quality signal: near-zero for a genuine match, large for a bad
    stereo correspondence)."""
    d1 = d1 / np.linalg.norm(d1)
    d2 = d2 / np.linalg.norm(d2)
    n = np.cross(d1, d2)
    nn = n @ n
    if nn < 1e-12:  # near-parallel rays, degenerate
        return None, np.inf
    t1 = np.linalg.det(np.array([o2 - o1, d2, n])) / nn
    t2 = np.linalg.det(np.array([o2 - o1, d1, n])) / nn
    p1 = o1 + t1 * d1
    p2 = o2 + t2 * d2
    return (p1 + p2) / 2, float(np.linalg.norm(p1 - p2))


def epipolar_score(oA, dA, oB, dB_all):
    """For B's ray to intersect (or nearly intersect) A's ray, dB must lie in the plane spanned by
    dA and the baseline (oB-oA); n is that plane's normal. |dB . n| is then the sine of B's ray's
    angle out of the plane -- near 0 for a genuine match, expressed without an explicit essential
    matrix since both rays are already in a common (IMU) frame."""
    n = np.cross(dA, (oB - oA))
    nn = np.linalg.norm(n)
    if nn < 1e-9:
        return np.zeros(len(dB_all))
    n = n / nn
    return np.abs(dB_all @ n)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('cap_dir')
    ap.add_argument('calib')
    ap.add_argument('camA', type=int)
    ap.add_argument('camB', type=int)
    ap.add_argument('out_model')
    ap.add_argument('--expo-us', type=float, default=None,
                     help='exposure class to use, microseconds; default = shortest present')
    ap.add_argument('--max-lag-ns', type=int, default=100_000)
    ap.add_argument('--epi-thresh', type=float, default=0.01,
                     help='max epipolar-plane distance (unit bearing space) to accept a match')
    ap.add_argument('--outlier-mad', type=float, default=4.0,
                     help='reject triangulated points beyond this many median-absolute-deviations '
                          'from the median point (rigid constellation should cluster tightly)')
    args = ap.parse_args()

    c = json.load(open(args.calib))['value0']
    TA, TB = se3(c['T_imu_cam'][args.camA]), se3(c['T_imu_cam'][args.camB])
    intrA, intrB = c['intrinsics'][args.camA]['intrinsics'], c['intrinsics'][args.camB]['intrinsics']
    oA, oB = TA[:3, 3], TB[:3, 3]

    rows = load_index(f'{args.cap_dir}/frames.idx')
    expos = sorted(set(round(r['expo'] * 1e6, 1) for r in rows))
    expo_us = args.expo_us if args.expo_us is not None else expos[0]
    print(f"exposure classes present (us): {expos}  -- using {expo_us}")
    sel = [r for r in rows if abs(r['expo']*1e6 - expo_us) < 1]
    ca = sorted([r for r in sel if r['cam'] == args.camA], key=lambda r: r['ts'])
    cb = sorted([r for r in sel if r['cam'] == args.camB], key=lambda r: r['ts'])
    print(f"cam{args.camA}: {len(ca)} frames   cam{args.camB}: {len(cb)} frames")
    if not ca or not cb:
        sys.exit("no frames in the requested exposure class for one or both cameras")

    blob = open(f'{args.cap_dir}/frames.bin', 'rb')

    # Exclude any IR source that doesn't move across the capture -- most plausibly the OTHER
    # controller sitting idle in view rather than the one being moved for this capture
    # (research-notes/58 found this created an incoherent, wrongly-scaled model by mixing points
    # from two different rigid bodies). The camera itself is fixed all session, so real motion is
    # the only thing that distinguishes "the tracked controller" from "anything else IR-bright".
    static_A = find_static_positions(lambda r: read_frame(blob, r['off'], r['size']), ca)
    static_B = find_static_positions(lambda r: read_frame(blob, r['off'], r['size']), cb)
    print(f"static (non-moving) sources excluded: cam{args.camA}={len(static_A)} "
          f"cam{args.camB}={len(static_B)}")

    best = None
    tb_ts = np.array([r['ts'] for r in cb])
    for ra in ca:
        j = int(np.argmin(np.abs(tb_ts - ra['ts'])))
        rb = cb[j]
        if abs(rb['ts'] - ra['ts']) > args.max_lag_ns:
            continue
        imA = read_frame(blob, ra['off'], ra['size'])
        imB = read_frame(blob, rb['off'], rb['size'])
        if imA is None or imB is None:
            continue
        bA = filter_static(find_blobs(imA), static_A)
        bB = filter_static(find_blobs(imB), static_B)
        if len(bA) < 2 or len(bB) < 2:
            continue
        score = min(len(bA), len(bB))
        if best is None or score > best[0]:
            best = (score, ra, rb, bA, bB)

    if best is None:
        sys.exit("no usable stereo pair found (need >=2 blobs in both cameras, close in time)")
    score, ra, rb, bA, bB = best
    print(f"best pair: cam{args.camA} fno={ra['fno']} ts={ra['ts']}  "
          f"cam{args.camB} fno={rb['fno']} ts={rb['ts']}  dt={rb['ts']-ra['ts']} ns  "
          f"blobs {len(bA)}/{len(bB)}")

    dirsA = kb4_unproject(bA[:, :2], intrA)
    dirsB = kb4_unproject(bB[:, :2], intrB)
    dirsA_imu = (TA[:3, :3] @ dirsA.T).T
    dirsB_imu = (TB[:3, :3] @ dirsB.T).T

    # Brute-force stereo matching: small blob counts (<20 both sides), so all-pairs epipolar
    # scoring is cheap and exact rather than needing a fast approximate matcher.
    points, matches = [], []
    used_b = set()
    for i, dA in enumerate(dirsA_imu):
        scores = epipolar_score(oA, dA, oB, dirsB_imu)
        order = np.argsort(scores)
        for j in order:
            if j in used_b or scores[j] > args.epi_thresh:
                continue
            p, gap = triangulate(oA, dA, oB, dirsB_imu[j])
            if p is None:
                continue
            points.append(p)
            matches.append((i, int(j), scores[j], gap))
            used_b.add(j)
            break

    if not points:
        sys.exit("no stereo matches passed the epipolar threshold -- try --epi-thresh looser")

    points = np.array(points)
    print(f"triangulated {len(points)} LED points (pre-outlier-rejection)")
    for (i, j, esc, gap), p in zip(matches, points):
        print(f"  A#{i} <-> B#{j}  epipolar={esc:.5f}  ray-gap={gap*1000:.2f} mm  "
              f"p=({p[0]:+.4f}, {p[1]:+.4f}, {p[2]:+.4f}) m")

    # Epipolar consistency alone accepts occasional wrong correspondences: with several blobs
    # clustered in a small image region, a mismatched pair can still lie close to the true
    # epipolar line (small angular residual) while triangulating to a wildly different depth --
    # observed directly on this capture (one match landed ~3x farther than the rest). A rigid LED
    # constellation must cluster tightly, so reject points far from the median as a robust filter;
    # RANSAC on the whole match set would be the principled version, cheap median filtering is
    # enough given how starkly the one outlier stood out here.
    med = np.median(points, axis=0)
    dist = np.linalg.norm(points - med, axis=1)
    mad = np.median(dist) + 1e-9
    keep = dist < args.outlier_mad * mad
    if not keep.all():
        print(f"rejecting {(~keep).sum()} outlier(s) beyond {args.outlier_mad}x the median "
              f"distance from the median point:")
        for (i, j, *_), p, d in zip(np.array(matches, dtype=object)[~keep], points[~keep],
                                     dist[~keep]):
            print(f"  A#{i} <-> B#{j}  p=({p[0]:+.4f}, {p[1]:+.4f}, {p[2]:+.4f})  "
                  f"{d*1000:.0f} mm from median")
    points = points[keep]

    centroid = points.mean(0)
    spread = np.linalg.norm(points - centroid, axis=1)
    print(f"kept {len(points)} points; spread from centroid: mean={spread.mean()*1000:.1f} mm  "
          f"max={spread.max()*1000:.1f} mm")

    json.dump({
        'source_cap_dir': args.cap_dir,
        'camA': args.camA, 'camB': args.camB,
        'frame': {'camA_fno': ra['fno'], 'camA_ts': ra['ts'],
                  'camB_fno': rb['fno'], 'camB_ts': rb['ts']},
        'points_imu_frame_at_capture': points.tolist(),
        'n_points': len(points),
    }, open(args.out_model, 'w'), indent=2)
    print(f"wrote {args.out_model}")


if __name__ == '__main__':
    main()
