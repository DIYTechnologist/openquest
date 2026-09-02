#!/usr/bin/env python3
"""make_synthetic_euroc.py — generate a synthetic EuRoC dataset with exact ground truth.

Purpose: separate "our harness is wrong" from "our data is wrong". Two unrelated estimators drift
on our real capture even though it measures as excellent on every axis (notes/12), which makes the
harness the leading suspect. A public EuRoC sequence would settle it, but the ETH host is
unreachable from here, so this generates an equivalent test locally.

Everything is analytic: a smooth trajectory, a fixed point cloud, a parallel stereo pinhole rig,
and IMU measurements derived from the same motion. If a VIO cannot track this, the fault is in the
harness or config, not the sensor data.

Also serves as a regression test for the whole EuRoC path (builder -> calibration -> runner).

Usage: make_synthetic_euroc.py <out_dir> [seconds] [cam_hz] [imu_hz]
"""
import json, math, os, sys
import numpy as np
import cv2

W, H, FOCAL = 640, 480, 190.0          # match the Quest rectified rig
BASELINE = 0.1117                      # metres, matches cam0<->cam2
G = 9.80665
TIME_BASE_NS = 100_000_000_000


def euler_R(yaw, pitch, roll):
    cy, sy = math.cos(yaw), math.sin(yaw)
    cp, sp = math.cos(pitch), math.sin(pitch)
    cr, sr = math.cos(roll), math.sin(roll)
    Rz = np.array([[cy, -sy, 0], [sy, cy, 0], [0, 0, 1]])
    Ry = np.array([[cp, 0, sp], [0, 1, 0], [-sp, 0, cp]])
    Rx = np.array([[1, 0, 0], [0, cr, -sr], [0, sr, cr]])
    return Rz @ Ry @ Rx


STILL_S = 2.0          # stationary period before motion starts
CAM_START_S = 0.5      # IMU lead before the first camera frame


def ramp(t):
    """0 while stationary, then a quick smooth ramp to full motion.

    Every VIO needs a well-conditioned start. OpenVINS' default STATIC initialiser wants the
    device still and then a distinct 'jerk' into motion; if it instead fires mid-motion it
    initialises with zero velocity and gravity aligned to an accelerometer reading that already
    contains real acceleration, and the estimate diverges. Our first real capture began with the
    headset already moving, which is exactly that failure.
    """
    if t <= STILL_S:
        return 0.0
    x = min(1.0, (t - STILL_S) / 0.4)
    return x * x * (3 - 2 * x)          # smoothstep


def pose(t):
    """Body pose: stationary, then gentle translation with mild rotation and real parallax."""
    w, k = 0.6, ramp(t)
    p = k * np.array([0.8*math.sin(w*(t-STILL_S)), 0.5*math.sin(0.7*w*(t-STILL_S)),
                      0.15*math.sin(1.3*w*(t-STILL_S))])
    R = euler_R(k*0.35*math.sin(0.45*(t-STILL_S)), k*0.20*math.sin(0.6*(t-STILL_S)),
                k*0.12*math.sin(0.8*(t-STILL_S)))
    return p, R


def accel_world(t, h=1e-4):
    p1, _ = pose(t - h)
    p2, _ = pose(t)
    p3, _ = pose(t + h)
    return (p1 - 2*p2 + p3) / (h*h)


def omega_body(t, h=1e-4):
    _, R1 = pose(t)
    _, R2 = pose(t + h)
    dR = R1.T @ R2
    ang = math.acos(max(-1.0, min(1.0, (np.trace(dR) - 1) / 2)))
    if ang < 1e-12:
        return np.zeros(3)
    axis = np.array([dR[2, 1]-dR[1, 2], dR[0, 2]-dR[2, 0], dR[1, 0]-dR[0, 1]]) / (2*math.sin(ang))
    return axis * ang / h


PATCH = 7      # side of the per-landmark appearance patch


def make_patches(n, rng):
    """A distinct random patch per landmark.

    Identical circles were a real flaw in an earlier version of this fixture: every landmark looked
    the same, so KLT could match one blob onto a different blob, report success, and hand the
    estimator confidently WRONG correspondences. Real scenes are locally distinctive; the fixture
    has to be too, or it validates nothing.
    """
    p = rng.integers(40, 255, size=(n, PATCH, PATCH)).astype(np.float32)
    return [cv2.GaussianBlur(x, (3, 3), 0) for x in p]


def render(P_world, p, R, cam_offset, rng, patches):
    """Project the cloud into one camera, splatting each landmark's own patch."""
    img = np.full((H, W), 12, np.float32)
    C = p + R @ cam_offset                      # camera centre in world
    pc = (R.T @ (P_world - C).T).T              # camera frame (camera axes == body axes)
    z = pc[:, 2]
    idx = np.nonzero(z > 0.4)[0]
    u = FOCAL * pc[idx, 0] / z[idx] + W/2
    v = FOCAL * pc[idx, 1] / z[idx] + H/2
    h = PATCH // 2
    for j, uu, vv in zip(idx, u, v):
        cu, cv_ = int(round(uu)), int(round(vv))
        if cu < h or cu >= W-h or cv_ < h or cv_ >= H-h:
            continue
        sl = (slice(cv_-h, cv_+h+1), slice(cu-h, cu+h+1))
        img[sl] = np.maximum(img[sl], patches[j])
    img = cv2.GaussianBlur(img, (3, 3), 0)
    img += rng.normal(0, 1.5, (H, W))
    return np.clip(img, 0, 255).astype(np.uint8)


def main(out, seconds, cam_hz, imu_hz):
    rng = np.random.default_rng(7)
    # a cloud spread in depth so there is real parallax, not a plane
    P = np.column_stack([rng.uniform(-6, 6, 900), rng.uniform(-4, 4, 900), rng.uniform(2, 12, 900)])
    patches = make_patches(len(P), rng)

    for c in ('cam0', 'cam1'):
        os.makedirs(f'{out}/mav0/{c}/data', exist_ok=True)
    os.makedirs(f'{out}/mav0/imu0', exist_ok=True)

    # IMU. Generated to run PAST the last camera frame: the estimator propagates across the
    # interval ending at each image time and interpolates the bounding samples, so a frame with no
    # IMU after it produces "No IMU measurements to propagate with ... IMU-CAMERA are likely
    # messed up". Cameras start at CAM_START_S and run `seconds`, so the IMU covers a margin beyond.
    with open(f'{out}/mav0/imu0/data.csv', 'w') as f:
        f.write('#timestamp [ns],w_x,w_y,w_z,a_x,a_y,a_z\n')
        n = int((seconds + CAM_START_S + 1.0) * imu_hz)
        for k in range(n):
            t = k / imu_hz
            _, R = pose(t)
            w = omega_body(t)
            # accelerometer measures specific force: R^T (a_world - g_world), g pointing down
            a = R.T @ (accel_world(t) + np.array([0, 0, G]))
            ts = TIME_BASE_NS + int(t * 1e9)
            f.write('%d,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g\n' % (ts, w[0], w[1], w[2], a[0], a[1], a[2]))

    # cameras + ground-truth poses
    offs = [np.array([0.0, 0, 0]), np.array([BASELINE, 0, 0])]
    csv = {c: open(f'{out}/mav0/{c}/data.csv', 'w') for c in ('cam0', 'cam1')}
    for c in csv.values():
        c.write('#timestamp [ns],filename\n')
    gt = open(f'{out}/ground_truth.txt', 'w')
    gt.write('# timestamp tx ty tz\n')
    nf = int(seconds * cam_hz)
    for k in range(nf):
        t = CAM_START_S + k / cam_hz       # start after some IMU lead
        p, R = pose(t)
        ts = TIME_BASE_NS + int(t * 1e9)
        for idx, c in enumerate(('cam0', 'cam1')):
            cv2.imwrite(f'{out}/mav0/{c}/data/{ts}.png', render(P, p, R, offs[idx], rng, patches))
            csv[c].write(f'{ts},{ts}.png\n')
        gt.write('%.9f %.6f %.6f %.6f\n' % (ts*1e-9, p[0], p[1], p[2]))
    for c in csv.values():
        c.close()
    gt.close()

    # Basalt-format calibration (identity camera<->IMU rotation keeps the test unambiguous)
    cal = {"value0": {
        "T_imu_cam": [{"px": float(o[0]), "py": 0.0, "pz": 0.0,
                       "qx": 0.0, "qy": 0.0, "qz": 0.0, "qw": 1.0} for o in offs],
        "intrinsics": [{"camera_type": "pinhole",
                        "intrinsics": {"fx": FOCAL, "fy": FOCAL, "cx": W/2-0.5, "cy": H/2-0.5}}
                       for _ in offs],
        "resolution": [[W, H], [W, H]], "vignette": [],
        "calib_accel_bias": [0.0]*9, "calib_gyro_bias": [0.0]*12,
        "imu_update_rate": float(imu_hz),
        "accel_noise_std": [0.016]*3, "gyro_noise_std": [0.000282]*3,
        "accel_bias_std": [0.001]*3, "gyro_bias_std": [0.0001]*3,
        "cam_time_offset_ns": 0}}
    json.dump(cal, open(f'{out}/calib.json', 'w'), indent=1)
    print(f"wrote {nf} stereo frames + {int(seconds*imu_hz)} imu samples to {out}")
    print(f"ground truth path length = %.2f m" %
          sum(np.linalg.norm(pose(0.5+(k+1)/cam_hz)[0] - pose(0.5+k/cam_hz)[0]) for k in range(nf-1)))


if __name__ == '__main__':
    main(sys.argv[1],
         float(sys.argv[2]) if len(sys.argv) > 2 else 20.0,
         float(sys.argv[3]) if len(sys.argv) > 3 else 30.0,
         float(sys.argv[4]) if len(sys.argv) > 4 else 1000.0)
