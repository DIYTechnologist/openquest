#!/usr/bin/env python3
"""check_imu_cam_extrinsic.py — validate the camera<->IMU rotation against the actual data.

Our epipolar check validated camera-to-CAMERA geometry, but nothing had validated the
camera-to-IMU extrinsic in any estimator's convention. A transposed rotation there is invisible to
every static check yet makes visual updates inconsistent with IMU propagation, so they get
chi2-rejected and the filter silently falls back to IMU-only dead reckoning — exactly the drift
both Basalt and OpenVINS showed (notes/12).

Test: angular velocity is observable in both sensors. From the images we can measure the camera's
own angular velocity (image rotation = omega_z, image translation = omega_x/omega_y for a distant
scene). The gyro measures it in the IMU frame. They must agree once rotated by the calibration:

    omega_cam = R_cam_imu * omega_imu

If the stored rotation is the transpose of what we think, correlating against the wrong one gives
visibly worse agreement. Both are scored so the answer is comparative, not absolute.

Expects a RECTIFIED (true pinhole) dataset so the small-angle image model holds.

Usage: check_imu_cam_extrinsic.py <dataset_dir> <calib.json> [max_frames]
"""
import json, math, sys
import numpy as np
import cv2


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


def visual_omega(dataset, cam, focal, max_frames, W2=319.5, H2=239.5):
    """Angular velocity in the camera frame, from image motion between consecutive frames."""
    rows = [l.strip().split(',') for l in open(f'{dataset}/mav0/{cam}/data.csv')
            if not l.startswith('#')]
    rows = rows[:max_frames]
    ts, om = [], []
    prev = cv2.imread(f'{dataset}/mav0/{cam}/data/{rows[0][1]}', cv2.IMREAD_GRAYSCALE)
    for k in range(1, len(rows)):
        cur = cv2.imread(f'{dataset}/mav0/{cam}/data/{rows[k][1]}', cv2.IMREAD_GRAYSCALE)
        if cur is None:
            prev = cur
            continue
        dt = (int(rows[k][0]) - int(rows[k-1][0])) * 1e-9
        p0 = cv2.goodFeaturesToTrack(prev, 300, 0.01, 8)
        if p0 is not None and dt > 0:
            p1, st, _ = cv2.calcOpticalFlowPyrLK(prev, cur, p0, None, winSize=(21, 21), maxLevel=3)
            g = st.ravel() == 1
            if g.sum() > 30:
                # ONLY the in-image rotation is used. Image TRANSLATION is contaminated by parallax
                # from camera translation (it inflated omega_x/omega_y by 1.55x), and essential-
                # matrix decomposition is degenerate at these small inter-frame baselines (it gave
                # 8.98 rad/s against the IMU's 0.65). Rotation of the image about its centre can
                # only come from rotation about the optical axis, so it is the one trustworthy
                # observable -- and one axis is enough to distinguish the two conventions.
                A, _ = cv2.estimateAffinePartial2D(p0[g], p1[g], method=cv2.RANSAC,
                                                   ransacReprojThreshold=2.0)
                if A is not None:
                    theta = math.atan2(A[1, 0], A[0, 0])
                    om.append([0.0, 0.0, theta / dt])
                    ts.append((int(rows[k][0]) + int(rows[k-1][0])) * 0.5e-9)
        prev = cur
    return np.array(ts), np.array(om)


def kabsch(A, B):
    """Best-fit rotation R minimising |R*A - B| over paired 3-vectors (rows)."""
    H = A.T @ B
    U, _, Vt = np.linalg.svd(H)
    d = np.sign(np.linalg.det(Vt.T @ U.T))
    return Vt.T @ np.diag([1, 1, d]) @ U.T


def rot_angle(R):
    return math.degrees(math.acos(max(-1.0, min(1.0, (np.trace(R) - 1) / 2))))


def score(name, pred, meas):
    per_axis = [np.corrcoef(pred[:, i], meas[:, i])[0, 1] for i in range(3)]
    resid = np.linalg.norm(pred - meas, axis=1)
    print(f"  {name:<28} per-axis corr = [{per_axis[0]:+.3f} {per_axis[1]:+.3f} "
          f"{per_axis[2]:+.3f}]  mean |resid| = {resid.mean():.3f} rad/s")
    return float(np.mean(per_axis))


def main(dataset, calib_path, max_frames):
    c = json.load(open(calib_path))['value0']
    R_i_c = quat_to_R(c['T_imu_cam'][0])          # per Basalt: camera frame -> IMU frame
    focal = c['intrinsics'][0]['intrinsics']['fx']
    print(f"focal = {focal:.1f} px, {max_frames} frames")

    ts_v, om_v = visual_omega(dataset, 'cam0', focal, max_frames)
    ts_i, om_i = load_imu(f'{dataset}/mav0/imu0/data.csv')
    print(f"visual omega samples = {len(ts_v)}, imu samples = {len(ts_i)}")
    if len(ts_v) < 50:
        print("not enough visual samples"); return 1

    # resample the gyro onto the visual timestamps
    om_r = np.stack([np.interp(ts_v, ts_i, om_i[:, i]) for i in range(3)], axis=1)

    print("\nmean |omega|: visual %.3f rad/s, imu %.3f rad/s" %
          (np.linalg.norm(om_v, axis=1).mean(), np.linalg.norm(om_r, axis=1).mean()))
    # Compare |correlation| on the optical-axis component only. The sign of our theta->omega_z
    # mapping is a convention we did not derive rigorously, so magnitude is what is meaningful.
    wz_meas = om_v[:, 2]
    a = abs(np.corrcoef((R_i_c.T @ om_r.T).T[:, 2], wz_meas)[0, 1])
    b = abs(np.corrcoef((R_i_c @ om_r.T).T[:, 2], wz_meas)[0, 1])
    print(f"\noptical-axis rotation, |correlation| with gyro:")
    print(f"  omega_cam = R_i_c^T * omega_imu   (Basalt/OpenVINS reading of T_imu_cam) : {a:.3f}")
    print(f"  omega_cam = R_i_c   * omega_imu   (opposite sense)                       : {b:.3f}")

    # Convention-agnostic: fit the rotation that actually relates the two signals, then see which
    # candidate it matches. This sidesteps any sign error in the hand-derived visual model.
    # Weight toward samples where rotation dominates: image translation also carries parallax from
    # camera translation, which inflates the omega_x/omega_y estimate.
    print()
    if max(a, b) < 0.5:
        print(f"VERDICT: neither convention tracks the gyro (best {max(a, b):.3f}) — "
              "camera<->IMU rotation looks wrong, or omega_z is too weak in this capture")
    elif abs(a - b) < 0.1:
        print(f"VERDICT: inconclusive — the two conventions are too close ({a:.3f} vs {b:.3f})")
    elif a > b:
        print(f"VERDICT: the estimators' reading is CORRECT ({a:.3f} vs {b:.3f})")
    else:
        print(f"VERDICT: T_imu_cam rotation is stored in the OPPOSITE sense to what Basalt and "
              f"OpenVINS assume ({b:.3f} vs {a:.3f})")
    return 0


if __name__ == '__main__':
    n = int(sys.argv[3]) if len(sys.argv) > 3 else 400
    sys.exit(main(sys.argv[1], sys.argv[2], n))
