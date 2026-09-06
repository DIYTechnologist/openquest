#!/usr/bin/env python3
"""lag_and_tracking_profile.py — is the residual divergence timing jitter, or bad tracking?

notes/51 established that leech frame timestamps are 100-250 ms early relative to the IMU clock,
and that a single global shift fixes the in-place capture (7.6 cm ATE) but only partly fixes the
room-scale walking capture (7562 m -> ~2839 m). Two candidates were left open:

  (a) the lag is not constant -- a single correction then fails exactly during fast motion;
  (b) feature tracking itself degrades under real walking motion (blur, large displacements).

This measures both from one pass over the images, so they can be compared on the same time axis.

Per consecutive frame pair it records:
  omega_z   in-plane rotation rate from estimateAffinePartial2D (the one observable immune to
            translation parallax -- see check_imu_cam_extrinsic.py)
  n_track   KLT survivors, and survival fraction of the detected corners
  inl       RANSAC inlier fraction of the affine fit (tracking self-consistency)
  flow      median pixel displacement (how hard the tracking problem is)
  blur      variance of Laplacian (focus/motion-blur proxy; lower = blurrier)

Then it sweeps the camera->IMU lag in ROLLING WINDOWS, not once globally. A constant offset shows
a flat lag-vs-time line; jitter shows it wandering, and wandering correlated with motion speed is
the smoking gun for (a).

Expects a RECTIFIED dataset (true pinhole) so the small-angle image model holds.

Usage: lag_and_tracking_profile.py <rect_dataset> <calib.json> [--win 12] [--step 3]
                                   [--lag-min -0.40] [--lag-max 0.10] [--cache f.npz]
"""
import argparse
import json
import math
import os
import sys

import cv2
import numpy as np

MAX_OMEGA = 8.0        # rad/s; above this the affine fit is degenerate (notes/51)
MIN_INLIERS = 20


def quat_to_R(d):
    q = np.array([d['qx'], d['qy'], d['qz'], d['qw']], float)
    q /= np.linalg.norm(q)
    x, y, z, w = q
    return np.array([
        [1-2*(y*y+z*z), 2*(x*y-z*w),   2*(x*z+y*w)],
        [2*(x*y+z*w),   1-2*(x*x+z*z), 2*(y*z-x*w)],
        [2*(x*z-y*w),   2*(y*z+x*w),   1-2*(x*x+y*y)]])


def load_imu(path):
    t, w = [], []
    for line in open(path):
        if line.startswith('#'):
            continue
        c = line.strip().split(',')
        t.append(int(c[0]) * 1e-9)
        w.append([float(c[1]), float(c[2]), float(c[3])])
    return np.array(t), np.array(w)


def profile_frames(dataset, cam='cam0'):
    rows = [l.strip().split(',') for l in open(f'{dataset}/mav0/{cam}/data.csv')
            if not l.startswith('#')]
    out = {k: [] for k in ('t', 'omega_z', 'n_det', 'n_track', 'inl', 'flow', 'blur', 'ok')}
    prev = cv2.imread(f'{dataset}/mav0/{cam}/data/{rows[0][1]}', cv2.IMREAD_GRAYSCALE)
    for k in range(1, len(rows)):
        cur = cv2.imread(f'{dataset}/mav0/{cam}/data/{rows[k][1]}', cv2.IMREAD_GRAYSCALE)
        if cur is None:
            prev = cur
            continue
        if k % 200 == 0:
            print(f"  frame {k}/{len(rows)}", file=sys.stderr)
        dt = (int(rows[k][0]) - int(rows[k-1][0])) * 1e-9
        t_mid = (int(rows[k][0]) + int(rows[k-1][0])) * 0.5e-9
        blur = float(cv2.Laplacian(cur, cv2.CV_64F).var())
        p0 = cv2.goodFeaturesToTrack(prev, 300, 0.01, 8)
        n_det = 0 if p0 is None else len(p0)
        n_track, inl, flow, omz, ok = 0, 0.0, 0.0, np.nan, False
        if p0 is not None and dt > 0:
            p1, st, _ = cv2.calcOpticalFlowPyrLK(prev, cur, p0, None,
                                                 winSize=(21, 21), maxLevel=3)
            g = st.ravel() == 1
            n_track = int(g.sum())
            if n_track > 30:
                a0, a1 = p0[g].reshape(-1, 2), p1[g].reshape(-1, 2)
                flow = float(np.median(np.linalg.norm(a1 - a0, axis=1)))
                A, mask = cv2.estimateAffinePartial2D(p0[g], p1[g], method=cv2.RANSAC,
                                                      ransacReprojThreshold=2.0)
                if A is not None and mask is not None:
                    n_inl = int(mask.sum())
                    inl = n_inl / float(n_track)
                    w = math.atan2(A[1, 0], A[0, 0]) / dt
                    omz = w
                    ok = abs(w) <= MAX_OMEGA and n_inl >= MIN_INLIERS
        for key, v in (('t', t_mid), ('omega_z', omz), ('n_det', n_det), ('n_track', n_track),
                       ('inl', inl), ('flow', flow), ('blur', blur), ('ok', ok)):
            out[key].append(v)
        prev = cur
    return {k: np.array(v) for k, v in out.items()}


def best_lag(t_v, wz_v, t_i, wz_i, lags):
    """Peak |correlation| of visual omega_z against gyro omega_z, over candidate lags."""
    best_r, best_l = -1.0, np.nan
    curve = []
    for lag in lags:
        g = np.interp(t_v + lag, t_i, wz_i)
        if np.std(g) < 1e-9 or np.std(wz_v) < 1e-9:
            curve.append(0.0)
            continue
        r = abs(np.corrcoef(g, wz_v)[0, 1])
        curve.append(r)
        if r > best_r:
            best_r, best_l = r, lag
    return best_l, best_r, np.array(curve)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('dataset')
    ap.add_argument('calib')
    ap.add_argument('--win', type=float, default=12.0, help='rolling window length, s')
    ap.add_argument('--step', type=float, default=3.0, help='window hop, s')
    ap.add_argument('--lag-min', type=float, default=-0.40)
    ap.add_argument('--lag-max', type=float, default=0.10)
    ap.add_argument('--lag-step', type=float, default=0.002)
    ap.add_argument('--cache', default=None, help='npz to store/reuse the image pass')
    args = ap.parse_args()

    if args.cache and os.path.exists(args.cache):
        print(f"reusing image pass from {args.cache}")
        p = dict(np.load(args.cache))
    else:
        print("image pass (KLT + affine + blur) ...")
        p = profile_frames(args.dataset)
        if args.cache:
            np.savez_compressed(args.cache, **p)

    c = json.load(open(args.calib))['value0']
    R_i_c = quat_to_R(c['T_imu_cam'][0])
    t_i, om_i = load_imu(f'{args.dataset}/mav0/imu0/data.csv')
    # notes/51: R_i_c^T is the correct (Basalt/OpenVINS) sense -- confirmed, not re-litigated here.
    wz_i = (R_i_c.T @ om_i.T).T[:, 2]

    ok = p['ok'].astype(bool)
    t_v, wz_v = p['t'][ok], p['omega_z'][ok]
    t0 = p['t'][0]
    print(f"\nframes={len(p['t'])}  usable pairs={ok.sum()} ({100.0*ok.mean():.1f} %)  "
          f"span={p['t'][-1]-t0:.1f} s")

    lags = np.arange(args.lag_min, args.lag_max + 1e-9, args.lag_step)
    gl, gr, _ = best_lag(t_v, wz_v, t_i, wz_i, lags)
    print(f"GLOBAL best lag = {gl*1e3:+.1f} ms   r = {gr:.3f}")

    print(f"\nrolling windows ({args.win:.0f} s, hop {args.step:.0f} s):")
    print(f"{'t0':>7} {'lag_ms':>8} {'r':>6} {'n':>5} {'|wz|':>6} {'flow':>6} "
          f"{'surv':>6} {'inl':>6} {'blur':>7} {'rej%':>6}")
    rows = []
    start = t0
    while start + args.win <= p['t'][-1]:
        end = start + args.win
        m = (t_v >= start) & (t_v < end)
        mall = (p['t'] >= start) & (p['t'] < end)
        if m.sum() >= 40:
            l, r, _ = best_lag(t_v[m], wz_v[m], t_i, wz_i, lags)
            wmag = float(np.mean(np.abs(wz_v[m])))
            surv = float(np.mean(p['n_track'][mall] / np.maximum(p['n_det'][mall], 1)))
            fl = float(np.nanmedian(p['flow'][mall]))
            inl = float(np.mean(p['inl'][mall]))
            bl = float(np.median(p['blur'][mall]))
            rej = 100.0 * (1.0 - ok[mall].mean())
            rows.append((start - t0, l, r, int(m.sum()), wmag, fl, surv, inl, bl, rej))
            print(f"{start-t0:7.1f} {l*1e3:8.1f} {r:6.3f} {int(m.sum()):5d} {wmag:6.2f} "
                  f"{fl:6.2f} {surv:6.3f} {inl:6.3f} {bl:7.1f} {rej:6.1f}")
        start += args.step

    if rows:
        a = np.array(rows)
        strong = a[a[:, 2] >= 0.6]          # only windows where the lag peak is meaningful
        print(f"\nwindows with r>=0.6: {len(strong)}/{len(a)}")
        if len(strong) >= 3:
            print(f"  lag  mean {strong[:,1].mean()*1e3:+.1f} ms  "
                  f"std {strong[:,1].std()*1e3:.1f} ms  "
                  f"range [{strong[:,1].min()*1e3:+.1f}, {strong[:,1].max()*1e3:+.1f}] ms")
            for name, col in (('flow', 5), ('|wz|', 4), ('blur', 8)):
                r = np.corrcoef(strong[:, col], strong[:, 1])[0, 1]
                print(f"  corr(lag, {name}) = {r:+.3f}")
    return 0


if __name__ == '__main__':
    sys.exit(main())
