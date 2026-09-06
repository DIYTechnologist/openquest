#!/usr/bin/env python3
"""ate.py — absolute trajectory error of an OpenVINS run against Meta's ground truth.

The step-2 deliverables (notes/18) are an ATE RMSE with a stated alignment method, a drift rate in
m/min, and an RPE over 1 s windows. notes/51 computed these inline; this makes the protocol
reproducible and identical between captures so numbers can be compared.

Alignment is Sim3 (Umeyama with scale) by default, the standard EuRoC/TUM protocol, and the
scale-forced SE3 number is reported alongside because a scale far from 1 means the accelerometer
excitation did not observe metric scale -- the trajectory shape can be right while the scale is not.

Usage: ate.py <openvins_traj.txt> <meta_poses.csv> [--max-dt 0.020] [--rpe-window 1.0]
"""
import argparse
import sys

import numpy as np


def umeyama(src, dst, with_scale=True):
    """Similarity transform mapping src onto dst (both N x 3): returns s, R, t."""
    mu_s, mu_d = src.mean(0), dst.mean(0)
    S, D = src - mu_s, dst - mu_d
    C = D.T @ S / len(src)
    U, d, Vt = np.linalg.svd(C)
    W = np.eye(3)
    if np.linalg.det(U) * np.linalg.det(Vt) < 0:
        W[2, 2] = -1
    R = U @ W @ Vt
    s = (d * np.diag(W)).sum() / (S ** 2).sum() * len(src) if with_scale else 1.0
    return s, R, mu_d - s * R @ mu_s


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('traj')
    ap.add_argument('meta')
    ap.add_argument('--max-dt', type=float, default=0.020)
    ap.add_argument('--rpe-window', type=float, default=1.0)
    args = ap.parse_args()

    est = np.loadtxt(args.traj)
    t, p = est[:, 0], est[:, 1:4]
    m = np.loadtxt(args.meta, delimiter=',', skiprows=1)
    tm, pm = m[:, 0] * 1e-9, m[:, 2:5]

    j = np.clip(np.searchsorted(tm, t), 1, len(tm) - 1)
    j = np.where(np.abs(tm[j] - t) < np.abs(tm[j - 1] - t), j, j - 1)
    dt = np.abs(tm[j] - t)
    ok = dt <= args.max_dt
    if ok.sum() < 20:
        print(f"only {ok.sum()} poses matched within {args.max_dt*1e3:.0f} ms -- clocks disagree?")
        return 1
    te, pe, pg = t[ok], p[ok], pm[j[ok]]
    print(f"matched poses     : {ok.sum()} / {len(t)} within {args.max_dt*1e3:.0f} ms "
          f"(median {np.median(dt[ok])*1e3:.1f} ms)")
    print(f"covered span      : {te[-1]-te[0]:.1f} s")
    print(f"ground-truth path : {np.linalg.norm(np.diff(pg, axis=0), axis=1).sum():.2f} m, "
          f"bbox {np.ptp(pg, axis=0)[0]:.2f} x {np.ptp(pg, axis=0)[1]:.2f} x "
          f"{np.ptp(pg, axis=0)[2]:.2f} m")

    # Read the scale before believing the Sim3 number. A trajectory that diverges to kilometres
    # gets *shrunk to a point* by a free scale, and then reports an "ATE" equal to nothing more
    # than the spread of the ground truth -- 0.128 m for a 4685 m estimate, in one real case here.
    # The SE3 number and the scale are what distinguish a good fit from a collapsed one.
    results = {}
    for name, ws in (('Sim3 (Umeyama, R+t+scale)', True), ('SE3  (scale forced to 1)', False)):
        s, R, tr = umeyama(pe, pg, ws)
        al = (s * (R @ pe.T).T + tr)
        e = np.linalg.norm(al - pg, axis=1)
        results[ws] = (s, np.sqrt((e ** 2).mean()))
        print(f"\nalignment         : {name}")
        print(f"  ATE RMSE        : {np.sqrt((e**2).mean()):.4f} m")
        print(f"  median / max    : {np.median(e):.4f} m / {e.max():.4f} m")
        if ws:
            print(f"  estimated scale : {s:.3f}")
            span = te[-1] - te[0]
            print(f"  drift rate      : {np.sqrt((e**2).mean())/(span/60.0):.4f} m/min")
            # RPE over fixed-time windows, alignment-free (relative displacement magnitudes)
            k = np.searchsorted(te, te + args.rpe_window)
            good = k < len(te)
            de = np.linalg.norm(al[k[good]] - al[good], axis=1)
            dg = np.linalg.norm(pg[k[good]] - pg[good], axis=1)
            r = de - dg
            print(f"  RPE {args.rpe_window:.0f} s windows: RMSE {np.sqrt((r**2).mean()):.4f} m, "
                  f"median |err| {np.median(np.abs(r)):.4f} m  (n={good.sum()})")

    scale, rmse_sim3 = results[True]
    rmse_se3 = results[False][1]
    est_path = np.linalg.norm(np.diff(pe, axis=0), axis=1).sum()
    if not 0.5 < scale < 2.0 or rmse_se3 > 10 * max(rmse_sim3, 1e-6):
        print(f"\n*** DIVERGED -- the Sim3 number above is meaningless. Estimated scale {scale:.3f} "
              f"and SE3 RMSE {rmse_se3:.1f} m mean the free scale shrank a divergent trajectory "
              f"(estimated path {est_path:.0f} m vs ground truth "
              f"{np.linalg.norm(np.diff(pg, axis=0), axis=1).sum():.1f} m) onto the truth. ***")
    return 0


if __name__ == '__main__':
    sys.exit(main())
