#!/usr/bin/env python3
"""epipolar_check.py — which factory camera pair (if any) matches a stereo image pair?

Matches features between two simultaneous frames, unprojects them through the KB4 model, and
scores every ordered pair of factory cameras by epipolar (Sampson-like) error. The correct pairing
should stand out clearly; if none does, the images are not geometrically consistent with the
calibration at all.

Usage: epipolar_check.py <basalt_calibration.json> <imgA.png> <imgB.png>
"""
import json, sys, math
import numpy as np
import cv2


def quat_to_R(qx, qy, qz, qw):
    n = math.sqrt(qx*qx + qy*qy + qz*qz + qw*qw)
    qx, qy, qz, qw = qx/n, qy/n, qz/n, qw/n
    return np.array([
        [1-2*(qy*qy+qz*qz), 2*(qx*qy-qz*qw),   2*(qx*qz+qy*qw)],
        [2*(qx*qy+qz*qw),   1-2*(qx*qx+qz*qz), 2*(qy*qz-qx*qw)],
        [2*(qx*qz-qy*qw),   2*(qy*qz+qx*qw),   1-2*(qx*qx+qy*qy)]])


def se3(d):
    T = np.eye(4)
    T[:3, :3] = quat_to_R(d['qx'], d['qy'], d['qz'], d['qw'])
    T[:3, 3] = [d['px'], d['py'], d['pz']]
    return T


def kb4_unproject(pts, intr):
    fx, fy, cx, cy = intr['fx'], intr['fy'], intr['cx'], intr['cy']
    k = [intr['k1'], intr['k2'], intr['k3'], intr['k4']]
    out = []
    for u, v in pts:
        x = (u - cx) / fx
        y = (v - cy) / fy
        rd = math.hypot(x, y)
        if rd < 1e-9:
            out.append([0, 0, 1.0]); continue
        th = rd
        for _ in range(20):                       # Newton solve rd = th*(1+k1 th^2+...)
            th2 = th*th
            f = th*(1 + k[0]*th2 + k[1]*th2**2 + k[2]*th2**3 + k[3]*th2**4) - rd
            d = 1 + 3*k[0]*th2 + 5*k[1]*th2**2 + 7*k[2]*th2**3 + 9*k[3]*th2**4
            th -= f/d
        s = math.sin(th)
        out.append([s*x/rd, s*y/rd, math.cos(th)])
    return np.array(out)


def main(calib, pa, pb):
    c = json.load(open(calib))['value0']
    T = [se3(t) for t in c['T_imu_cam']]
    intr = [i['intrinsics'] for i in c['intrinsics']]

    A = cv2.imread(pa, cv2.IMREAD_GRAYSCALE)
    B = cv2.imread(pb, cv2.IMREAD_GRAYSCALE)
    sift = cv2.SIFT_create(4000)
    ka, da = sift.detectAndCompute(A, None)
    kb, db = sift.detectAndCompute(B, None)
    print(f"features: A={len(ka)} B={len(kb)}")
    bf = cv2.BFMatcher()
    raw = bf.knnMatch(da, db, k=2)
    good = [m for m, n in raw if m.distance < 0.75 * n.distance]
    print(f"ratio-test matches: {len(good)}")
    if len(good) < 12:
        print("too few matches to judge"); return
    ptsA = np.float32([ka[m.queryIdx].pt for m in good])
    ptsB = np.float32([kb[m.trainIdx].pt for m in good])

    print(f"\n{'pair':>8} {'median epi err (deg)':>22} {'inliers<1deg':>14}")
    res = []
    for a in range(len(T)):
        for b in range(len(T)):
            if a == b:
                continue
            Tab = np.linalg.inv(T[a]) @ T[b]
            R, t = Tab[:3, :3], Tab[:3, 3]
            if np.linalg.norm(t) < 1e-9:
                continue
            tx = np.array([[0, -t[2], t[1]], [t[2], 0, -t[0]], [-t[1], t[0], 0]])
            E = tx @ R
            ba = kb4_unproject(ptsA, intr[a])
            bb = kb4_unproject(ptsB, intr[b])
            num = np.abs(np.einsum('ij,jk,ik->i', ba, E, bb))
            Eb = (E @ bb.T).T
            Ea = (E.T @ ba.T).T
            den = np.sqrt(Eb[:, 0]**2 + Eb[:, 1]**2) + np.sqrt(Ea[:, 0]**2 + Ea[:, 1]**2) + 1e-12
            err = np.degrees(num / den)
            res.append((float(np.median(err)), int((err < 1.0).sum()), a, b))
    res.sort()
    for med, inl, a, b in res:
        print(f"   ({a},{b}) {med:>20.3f} {inl:>14d}")
    best = res[0]
    print(f"\nbest pair: ({best[2]},{best[3]})  median {best[0]:.3f} deg, {best[1]} inliers of {len(good)}")


if __name__ == '__main__':
    main(sys.argv[1], sys.argv[2], sys.argv[3])
