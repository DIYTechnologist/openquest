#!/usr/bin/env python3
"""stereo_vo_check.py — an IMU-free metric trajectory, as a cross-check on VIO.

Why this exists: every VIO result we have shares one IMU, one initialisation and one calibration
file, so a systematic IMU-side or global-frame error is common-mode and invisible to any comparison
between them (notes/14). This estimates the trajectory from the IMAGES ALONE, taking metric scale
from the known stereo baseline instead of the accelerometer. Nothing here touches imu0/data.csv.

Method, per frame k (classic stereo VO, no bundle adjustment):
  1. detect corners in cam0[k]
  2. KLT cam0[k] -> cam1[k], forward-backward checked -> triangulate against the known baseline,
     giving metric 3D points in the cam0[k] frame
  3. KLT cam0[k] -> cam0[k+1], forward-backward checked
  4. solvePnPRansac(3D from step 2, 2D from step 3) -> metric relative pose k -> k+1
  5. chain

It drifts (no loop closure, no BA), so it is a cross-check, not a reference. What it is good for is
SCALE and SHAPE: if the accelerometer-derived scale in the VIO were wrong, this would disagree
proportionally.

Expects a RECTIFIED pinhole pair from rectify_pair.py (the raw pair is 19.6 deg divergent, which
KLT cannot match across).

Usage: stereo_vo_check.py <rect_dataset> [vio_trajectory.txt] [stride]
"""
import json, os, sys
import cv2
import numpy as np


def quat_to_R(d):
    q = np.array([d['qx'], d['qy'], d['qz'], d['qw']], float)
    q /= np.linalg.norm(q)
    x, y, z, w = q
    return np.array([
        [1-2*(y*y+z*z), 2*(x*y-z*w),   2*(x*z+y*w)],
        [2*(x*y+z*w),   1-2*(x*x+z*z), 2*(y*z-x*w)],
        [2*(x*z-y*w),   2*(y*z+x*w),   1-2*(x*x+y*y)]])


def umeyama(X, Y, with_scale):
    """R,t,s minimising |Y - (s R X + t)|. X,Y are Nx3."""
    mx, my = X.mean(0), Y.mean(0)
    Xc, Yc = X - mx, Y - my
    U, S, Vt = np.linalg.svd(Xc.T @ Yc / len(X))
    D = np.diag([1, 1, np.sign(np.linalg.det(U @ Vt))])
    R = (U @ D @ Vt).T
    s = (S * np.diag(D)).sum() / (Xc**2).sum() * len(X) if with_scale else 1.0
    return R, my - s * R @ mx, s


LK = dict(winSize=(21, 21), maxLevel=3,
          criteria=(cv2.TERM_CRITERIA_EPS | cv2.TERM_CRITERIA_COUNT, 30, 0.01))


def klt(a, b, pts, tol):
    """Forward-backward checked KLT. -> (tracked points, boolean mask)."""
    q, st, _ = cv2.calcOpticalFlowPyrLK(a, b, pts, None, **LK)
    if q is None:
        return None, np.zeros(len(pts), bool)
    bk, st2, _ = cv2.calcOpticalFlowPyrLK(b, a, q, None, **LK)
    d = np.linalg.norm((bk - pts).reshape(-1, 2), axis=1)
    return q, (st.ravel() == 1) & (st2.ravel() == 1) & (d < tol)


def main(ds, vio_path=None, stride=1):
    calib = json.load(open(os.path.join(ds, 'calib.json')))['value0']
    intr = calib['intrinsics'][0]['intrinsics']
    K = np.array([[intr['fx'], 0, intr['cx']], [0, intr['fy'], intr['cy']], [0, 0, 1]])
    T = calib['T_imu_cam']
    R_ic = [quat_to_R(t) for t in T]
    p_ic = [np.array([t['px'], t['py'], t['pz']]) for t in T]
    # X_1 = (R_1^T R_0) X_0 + R_1^T (p_0 - p_1)
    R_10 = R_ic[1].T @ R_ic[0]
    t_10 = R_ic[1].T @ (p_ic[0] - p_ic[1])
    print('baseline %.4f m; residual rotation between virtual cams %.3f deg'
          % (np.linalg.norm(t_10),
             np.degrees(np.arccos(np.clip((np.trace(R_10)-1)/2, -1, 1)))))

    P0 = K @ np.hstack([np.eye(3), np.zeros((3, 1))])
    P1 = K @ np.hstack([R_10, t_10.reshape(3, 1)])

    d0 = os.path.join(ds, 'mav0', 'cam0', 'data')
    d1 = os.path.join(ds, 'mav0', 'cam1', 'data')
    names = sorted(os.listdir(d0))
    ts = np.array([int(n[:-4]) for n in names]) * 1e-9

    C = np.eye(4)                      # pose of cam0[k] in the VO world frame
    traj = [(ts[0], C[:3, 3].copy())]
    nfail = 0
    kept = []
    for i in range(0, len(names) - stride, stride):
        a0 = cv2.imread(os.path.join(d0, names[i]), 0)
        a1 = cv2.imread(os.path.join(d1, names[i]), 0)
        b0 = cv2.imread(os.path.join(d0, names[i + stride]), 0)
        pts = cv2.goodFeaturesToTrack(a0, maxCorners=400, qualityLevel=0.01, minDistance=8)
        if pts is None or len(pts) < 30:
            nfail += 1
            continue
        # stereo match -> metric 3D
        q1, m1 = klt(a0, a1, pts, 1.0)
        # temporal match
        q2, m2 = klt(a0, b0, pts, 1.0)
        m = m1 & m2
        if m.sum() < 20:
            nfail += 1
            continue
        X = cv2.triangulatePoints(P0, P1, pts[m].reshape(-1, 2).T, q1[m].reshape(-1, 2).T)
        X = (X[:3] / X[3]).T
        good = (X[:, 2] > 0.15) & (X[:, 2] < 30)      # room-scale depths only
        if good.sum() < 20:
            nfail += 1
            continue
        ok, rvec, tvec, inl = cv2.solvePnPRansac(
            X[good].astype(np.float64), q2[m][good].reshape(-1, 2).astype(np.float64),
            K, None, flags=cv2.SOLVEPNP_ITERATIVE, reprojectionError=2.0, confidence=0.999,
            iterationsCount=200)
        if not ok or inl is None or len(inl) < 12:
            nfail += 1
            continue
        Rr, _ = cv2.Rodrigues(rvec)
        rel = np.eye(4); rel[:3, :3] = Rr; rel[:3, 3] = tvec.ravel()   # X_{k+1} = rel * X_k
        C = C @ np.linalg.inv(rel)
        traj.append((ts[i + stride], C[:3, 3].copy()))
        kept.append(len(inl))

    traj_t = np.array([t for t, _ in traj])
    traj_p = np.array([p for _, p in traj])
    print('VO: %d poses, %d failed steps, median %d PnP inliers'
          % (len(traj), nfail, int(np.median(kept)) if kept else 0))
    print('VO path length %.2f m, net displacement %.2f m'
          % (np.linalg.norm(np.diff(traj_p, axis=0), axis=1).sum(),
             np.linalg.norm(traj_p[-1] - traj_p[0])))

    out = os.path.join(ds, 'vo_trajectory.txt')
    np.savetxt(out, np.column_stack([traj_t, traj_p]), fmt='%.9f',
               header='timestamp tx ty tz   (stereo VO, metric from baseline, NO IMU)')
    print('wrote', out)

    if vio_path:
        v = np.loadtxt(vio_path)
        lo, hi = max(traj_t[0], v[0, 0]), min(traj_t[-1], v[-1, 0])
        g = np.arange(lo, hi, 1/30.)
        A = np.stack([np.interp(g, traj_t, traj_p[:, i]) for i in range(3)], 1)
        B = np.stack([np.interp(g, v[:, 0], v[:, i+1]) for i in range(3)], 1)
        print('\noverlap %.1f s, %d samples' % (hi - lo, len(g)))
        # The VO run covers the whole capture including the stationary lead-in, where it random-
        # walks; the overlap window starts at VIO init. Report both so the two are not confused.
        print('  within overlap: VO path %.2f m net %.2f m | VIO path %.2f m net %.2f m'
              % (np.linalg.norm(np.diff(A, axis=0), axis=1).sum(), np.linalg.norm(A[-1]-A[0]),
                 np.linalg.norm(np.diff(B, axis=0), axis=1).sum(), np.linalg.norm(B[-1]-B[0])))
        for lbl, ws in (('rigid (scale forced to 1)', False), ('similarity (scale fitted)', True)):
            R, t, s = umeyama(A, B, ws)
            r = np.linalg.norm(B - (s * (R @ A.T).T + t), axis=1)
            extra = '   scale = %.4f (%.2f%% from unity)' % (s, 100*abs(s-1)) if ws else ''
            print('  %-26s RMS %.3f m  median %.3f m%s' % (lbl, np.sqrt((r**2).mean()),
                                                           np.median(r), extra))
        # drift-immune scale: ratio of per-step displacement magnitudes
        da = np.linalg.norm(np.diff(A, axis=0), axis=1)
        db = np.linalg.norm(np.diff(B, axis=0), axis=1)
        k = (da > 0.002) & (db > 0.002)
        print('  median per-step displacement ratio VIO/VO = %.4f  (%d steps)'
              % (np.median(db[k] / da[k]), k.sum()))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1],
                  sys.argv[2] if len(sys.argv) > 2 else None,
                  int(sys.argv[3]) if len(sys.argv) > 3 else 1))
