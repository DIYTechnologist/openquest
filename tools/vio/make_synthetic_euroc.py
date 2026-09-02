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

# Bisection knobs. The fixture tracks; the real capture does not. Rather than guess at the
# difference, make the fixture progressively realistic and find the single change that breaks it.
IMU_NOISE = float(os.environ.get('SYN_IMU_NOISE', '0'))    # 1.0 = the real sensor's noise+bias
KB4       = os.environ.get('SYN_KB4', '')                  # path to the real calibration, or '' for ideal pinhole
EXTR      = os.environ.get('SYN_EXTR', '')                 # same path: also use the REAL camera<->IMU
                                                           # extrinsics (19.6 deg divergent pair)


def quat_R(d):
    q = np.array([d['qx'], d['qy'], d['qz'], d['qw']], float)
    q /= np.linalg.norm(q)
    x, y, z, w = q
    return np.array([
        [1-2*(y*y+z*z), 2*(x*y-z*w),   2*(x*z+y*w)],
        [2*(x*y+z*w),   1-2*(x*x+z*z), 2*(y*z-x*w)],
        [2*(x*z-y*w),   2*(y*z+x*w),   1-2*(x*x+y*y)]])


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


def project(pc, intr):
    """Project camera-frame points. Pinhole, or the real Kannala-Brandt fisheye when given one."""
    if intr is None:
        z = pc[:, 2]
        return FOCAL*pc[:, 0]/z + W/2, FOCAL*pc[:, 1]/z + H/2
    x, y, z = pc[:, 0], pc[:, 1], pc[:, 2]
    r = np.sqrt(x*x + y*y)
    th = np.arctan2(r, z)
    t2 = th*th
    td = th*(1 + intr['k1']*t2 + intr['k2']*t2**2 + intr['k3']*t2**3 + intr['k4']*t2**4)
    sc = np.where(r > 1e-9, td/np.maximum(r, 1e-12), 0.0)
    return intr['fx']*sc*x + intr['cx'], intr['fy']*sc*y + intr['cy']


def render(P_world, p, R, cam_offset, rng, patches, intr=None, R_ic=None):
    """Project the cloud into one camera, splatting each landmark's own patch.

    R_ic is the camera->IMU rotation; with it the camera axes differ from the body axes, which is
    the real rig (the two cameras diverge by 19.6 deg).
    """
    img = np.full((H, W), 12, np.float32)
    C = p + R @ cam_offset                      # camera centre in world
    Rwc = R if R_ic is None else R @ R_ic       # camera->world
    pc = (Rwc.T @ (P_world - C).T).T
    z = pc[:, 2]
    idx = np.nonzero(z > 0.4)[0]
    u, v = project(pc[idx], intr)
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


BIAS_A = np.array([0.03, -0.02, 0.04])      # m/s^2
BIAS_G = np.array([0.004, 0.002, -0.003])   # rad/s


def main(out, seconds, cam_hz, imu_hz):
    rng = np.random.default_rng(7)
    print(f"  IMU_NOISE={IMU_NOISE}  KB4={KB4}")
    # A shell of points AROUND the device, not just in front of it. A frontal cloud is invisible to
    # a camera with a real camera<->IMU rotation -- the Quest's cameras point outward, so the
    # earlier frontal cloud left them seeing 7 features instead of 300 and looked exactly like an
    # extrinsics bug. Depth is randomised so there is genuine parallax rather than a shell surface.
    n_pts = 2500
    dirs = rng.normal(size=(n_pts, 3))
    dirs /= np.linalg.norm(dirs, axis=1, keepdims=True)
    P = dirs * rng.uniform(2.0, 12.0, (n_pts, 1))
    patches = make_patches(len(P), rng)
    real_intr = None
    if KB4:
        rc = json.load(open(KB4))['value0']
        real_intr = [rc['intrinsics'][0]['intrinsics'], rc['intrinsics'][2]['intrinsics']]
        print("  using real KB4 fisheye intrinsics from", KB4)
    real_T = None; declared = None
    if EXTR:
        ec = json.load(open(EXTR))['value0']
        real_T = [dict(ec['T_imu_cam'][0]), dict(ec['T_imu_cam'][2])]
        mode = os.environ.get('SYN_EXTR_MODE', 'full')
        if mode == 'parallel':
            # real camera<->IMU rotation, but both cameras share cam0's orientation: isolates the
            # camera-to-IMU rotation from the 19.6 deg divergence between the two cameras
            for k in ('qx', 'qy', 'qz', 'qw'):
                real_T[1][k] = real_T[0][k]
        elif mode == 'pos':
            # real positions, identity rotations: isolates the divergence from everything else
            for t in real_T:
                t['qx'] = t['qy'] = t['qz'] = 0.0
                t['qw'] = 1.0
        print(f"  using real camera<->IMU extrinsics (mode={mode})")
        # The fixture renders with R_ic AND declares R_ic, so no convention mismatch is possible
        # unless the consumer reads the field in the opposite sense. This writes the TRANSPOSE into
        # the calibration while still rendering with the original: if that tracks, the consumer's
        # convention is the opposite of ours and we know exactly how to fix the real config.
        if os.environ.get('SYN_DECLARE_TRANSPOSE'):
            import copy
            declared = copy.deepcopy(real_T)
            for t in declared:
                t['qx'], t['qy'], t['qz'] = -t['qx'], -t['qy'], -t['qz']   # conjugate = transpose
            print("  DECLARING the transposed rotation in the calibration")
        else:
            declared = real_T

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
            if IMU_NOISE:
                # per-sample sigmas from the factory calibration, plus a constant bias of the
                # magnitude actually seen on this device
                a = a + rng.normal(0, 0.016*IMU_NOISE, 3) + BIAS_A*IMU_NOISE
                w = w + rng.normal(0, 0.000282*IMU_NOISE, 3) + BIAS_G*IMU_NOISE
            ts = TIME_BASE_NS + int(t * 1e9)
            f.write('%d,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g\n' % (ts, w[0], w[1], w[2], a[0], a[1], a[2]))

    # cameras + ground-truth poses
    if real_T:
        offs = [np.array([t['px'], t['py'], t['pz']]) for t in real_T]
        Rics = [quat_R(t) for t in real_T]
    else:
        offs = [np.array([0.0, 0, 0]), np.array([BASELINE, 0, 0])]
        Rics = [None, None]
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
            cv2.imwrite(f'{out}/mav0/{c}/data/{ts}.png',
                        render(P, p, R, offs[idx], rng, patches,
                               real_intr[idx] if real_intr else None, Rics[idx]))
            csv[c].write(f'{ts},{ts}.png\n')
        gt.write('%.9f %.6f %.6f %.6f\n' % (ts*1e-9, p[0], p[1], p[2]))
    for c in csv.values():
        c.close()
    gt.close()

    # Basalt-format calibration (identity camera<->IMU rotation keeps the test unambiguous)
    cal = {"value0": {
        "T_imu_cam": (declared if real_T else
                      [{"px": float(o[0]), "py": 0.0, "pz": 0.0,
                        "qx": 0.0, "qy": 0.0, "qz": 0.0, "qw": 1.0} for o in offs]),
        "intrinsics": ([{"camera_type": "kb4", "intrinsics": dict(i)} for i in real_intr]
                       if real_intr else
                       [{"camera_type": "pinhole",
                         "intrinsics": {"fx": FOCAL, "fy": FOCAL, "cx": W/2-0.5, "cy": H/2-0.5}}
                        for _ in offs]),
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
