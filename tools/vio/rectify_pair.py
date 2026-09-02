#!/usr/bin/env python3
"""rectify_pair.py — warp a Quest fisheye camera pair into virtual PARALLEL pinhole cameras.

Why: Basalt's optical-flow front end tracks a patch directly from the cam0 image into the cam1
image, which assumes near-parallel views. The Quest's cameras are 19.6 deg apart (best pair) up to
164 deg, so that match never succeeds — `obs_cam1` is 0 on every frame, no landmark is ever
triangulated, and the filter degenerates to IMU dead-reckoning. See notes/12.

What this does: rotates each camera by half the relative rotation, so both virtual cameras share
one orientation, and reprojects each fisheye image into a virtual pinhole with that orientation.

Deliberately NOT classic stereo rectification: we do not rotate the baseline onto the x axis.
Basalt does 2D patch tracking rather than scanline search, so parallel *orientation* is the part
that matters, and skipping the baseline alignment keeps noticeably more usable field of view.

Camera centres are unchanged, so the baseline (and therefore metric scale) is preserved; only the
orientation of each camera frame changes, which is folded into T_imu_cam.

Usage: rectify_pair.py <basalt_calibration.json> <camA> <camB> <in_dataset> <out_dataset>
                       [focal] [imu_template.json]

The 4-camera output of quest_calib_convert.py has no IMU noise/rate fields, and Basalt refuses a
calibration without them ("provided NVP (imu_update_rate) not found"), so those keys are copied
from a complete pair calibration if the source lacks them.
"""
import json, math, os, shutil, sys
import numpy as np
import cv2

W, H = 640, 480


def quat_to_R(d):
    q = np.array([d['qx'], d['qy'], d['qz'], d['qw']], dtype=float)
    q /= np.linalg.norm(q)
    x, y, z, w = q
    return np.array([
        [1-2*(y*y+z*z), 2*(x*y-z*w),   2*(x*z+y*w)],
        [2*(x*y+z*w),   1-2*(x*x+z*z), 2*(y*z-x*w)],
        [2*(x*z-y*w),   2*(y*z+x*w),   1-2*(x*x+y*y)]])


def R_to_quat(R):
    t = np.trace(R)
    if t > 0:
        s = math.sqrt(t + 1.0) * 2
        w = 0.25 * s; x = (R[2, 1]-R[1, 2])/s; y = (R[0, 2]-R[2, 0])/s; z = (R[1, 0]-R[0, 1])/s
    elif R[0, 0] > R[1, 1] and R[0, 0] > R[2, 2]:
        s = math.sqrt(1.0 + R[0, 0] - R[1, 1] - R[2, 2]) * 2
        w = (R[2, 1]-R[1, 2])/s; x = 0.25*s; y = (R[0, 1]+R[1, 0])/s; z = (R[0, 2]+R[2, 0])/s
    elif R[1, 1] > R[2, 2]:
        s = math.sqrt(1.0 + R[1, 1] - R[0, 0] - R[2, 2]) * 2
        w = (R[0, 2]-R[2, 0])/s; x = (R[0, 1]+R[1, 0])/s; y = 0.25*s; z = (R[1, 2]+R[2, 1])/s
    else:
        s = math.sqrt(1.0 + R[2, 2] - R[0, 0] - R[1, 1]) * 2
        w = (R[1, 0]-R[0, 1])/s; x = (R[0, 2]+R[2, 0])/s; y = (R[1, 2]+R[2, 1])/s; z = 0.25*s
    return dict(qx=x, qy=y, qz=z, qw=w)


def sqrt_rotation(R):
    """Rotation by half the angle about the same axis."""
    ang = math.acos(max(-1.0, min(1.0, (np.trace(R) - 1) / 2)))
    if abs(ang) < 1e-12:
        return np.eye(3)
    axis = np.array([R[2, 1]-R[1, 2], R[0, 2]-R[2, 0], R[1, 0]-R[0, 1]]) / (2*math.sin(ang))
    a = ang / 2
    K = np.array([[0, -axis[2], axis[1]], [axis[2], 0, -axis[0]], [-axis[1], axis[0], 0]])
    return np.eye(3) + math.sin(a)*K + (1-math.cos(a))*(K @ K)


def kb4_project(bearings, intr):
    """Unit bearings (N,3) in camera frame -> fisheye pixel coords (N,2)."""
    fx, fy, cx, cy = intr['fx'], intr['fy'], intr['cx'], intr['cy']
    k1, k2, k3, k4 = intr['k1'], intr['k2'], intr['k3'], intr['k4']
    x, y, z = bearings[:, 0], bearings[:, 1], bearings[:, 2]
    r = np.sqrt(x*x + y*y)
    theta = np.arctan2(r, z)
    t2 = theta*theta
    td = theta*(1 + k1*t2 + k2*t2**2 + k3*t2**3 + k4*t2**4)
    scale = np.where(r > 1e-9, td/np.maximum(r, 1e-12), 0.0)
    return np.stack([fx*scale*x + cx, fy*scale*y + cy], axis=1)


def build_map(R_cam_rect, intr, focal):
    """Pixel maps sampling the fisheye image for each virtual pinhole pixel."""
    u, v = np.meshgrid(np.arange(W, dtype=np.float64), np.arange(H, dtype=np.float64))
    x = (u - W/2 + 0.5)/focal
    y = (v - H/2 + 0.5)/focal
    b = np.stack([x.ravel(), y.ravel(), np.ones(x.size)], axis=1)
    b /= np.linalg.norm(b, axis=1, keepdims=True)
    b_cam = (R_cam_rect @ b.T).T                    # rectified frame -> original camera frame
    px = kb4_project(b_cam, intr)
    mx = px[:, 0].reshape(H, W).astype(np.float32)
    my = px[:, 1].reshape(H, W).astype(np.float32)
    return mx, my


def main(calib_path, a, b, src, dst, focal, tmpl=None):
    c = json.load(open(calib_path))
    v = c['value0']
    Ra, Rb = quat_to_R(v['T_imu_cam'][a]), quat_to_R(v['T_imu_cam'][b])
    ia = v['intrinsics'][a]['intrinsics']
    ib = v['intrinsics'][b]['intrinsics']

    R_ab = Ra.T @ Rb                                 # rotation from cam b frame to cam a frame
    M = sqrt_rotation(R_ab)                          # half of it
    ang = math.degrees(math.acos(max(-1, min(1, (np.trace(R_ab)-1)/2))))
    print(f"pair ({a},{b}) relative rotation {ang:.1f} deg -> each camera rotated {ang/2:.1f} deg")

    # R_cam_rect for each: rect frames end up sharing one orientation (Ra@M == Rb@M.T).
    maps = [build_map(M, ia, focal), build_map(M.T, ib, focal)]
    check = np.allclose(Ra @ M, Rb @ M.T, atol=1e-9)
    print(f"virtual cameras share orientation: {check}")

    # New calibration: pinhole intrinsics, T_imu_cam rotated, positions untouched (scale preserved).
    # Build on the TEMPLATE when given: cereal parses struct members positionally, so a calibration
    # assembled with a different key order fails with "provided NVP (vignette) not found". The
    # template also carries the IMU noise/rate fields the 4-camera factory output lacks.
    out = json.loads(json.dumps(json.load(open(tmpl)) if tmpl else c))
    ov = out['value0']
    for idx, (src_i, Rc) in enumerate(((a, M), (b, M.T))):
        t = v['T_imu_cam'][src_i]
        Rn = quat_to_R(t) @ Rc
        q = R_to_quat(Rn)
        ov['T_imu_cam'][idx] = dict(px=t['px'], py=t['py'], pz=t['pz'], **q)
        ov['intrinsics'][idx] = {'camera_type': 'pinhole',
                                 'intrinsics': {'fx': focal, 'fy': focal,
                                                'cx': W/2 - 0.5, 'cy': H/2 - 0.5}}
        ov['resolution'][idx] = [W, H]
    ov['T_imu_cam'] = ov['T_imu_cam'][:2]
    ov['intrinsics'] = ov['intrinsics'][:2]
    ov['resolution'] = ov['resolution'][:2]
    if 'vignette' in ov:
        ov['vignette'] = []

    os.makedirs(dst, exist_ok=True)
    json.dump(out, open(os.path.join(dst, 'calib.json'), 'w'), indent=1)

    os.makedirs(os.path.join(dst, 'mav0', 'imu0'), exist_ok=True)
    shutil.copy(os.path.join(src, 'mav0', 'imu0', 'data.csv'),
                os.path.join(dst, 'mav0', 'imu0', 'data.csv'))

    for idx, cam in enumerate(('cam0', 'cam1')):
        sd = os.path.join(src, 'mav0', cam, 'data')
        dd = os.path.join(dst, 'mav0', cam, 'data')
        os.makedirs(dd, exist_ok=True)
        shutil.copy(os.path.join(src, 'mav0', cam, 'data.csv'),
                    os.path.join(dst, 'mav0', cam, 'data.csv'))
        mx, my = maps[idx]
        n = 0
        for fn in os.listdir(sd):
            img = cv2.imread(os.path.join(sd, fn), cv2.IMREAD_GRAYSCALE)
            if img is None:
                continue
            cv2.imwrite(os.path.join(dd, fn), cv2.remap(img, mx, my, cv2.INTER_LINEAR))
            n += 1
        print(f"  {cam}: rectified {n} images")
    print(f"wrote {dst}")


if __name__ == '__main__':
    f = float(sys.argv[6]) if len(sys.argv) > 6 else 190.0
    t = sys.argv[7] if len(sys.argv) > 7 else None
    main(sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), sys.argv[4], sys.argv[5], f, t)
