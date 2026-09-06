#!/usr/bin/env python3
"""make_openvins_config.py — generate an OpenVINS config for a Quest camera pair.

OpenVINS reads Kalibr-style YAML. Two things differ from what we already have:
  * it wants T_imu_cam (camera -> IMU); our Kalibr export stores T_cam_imu, so it is inverted here
  * it names the Kannala-Brandt model "equidistant", which is exactly our KB4 fit

Because OpenVINS models fisheye natively and does not require near-parallel cameras, this feeds
the ORIGINAL fisheye images -- no rectification and no loss of field of view, unlike the Basalt
path (see notes/12).

Usage: make_openvins_config.py <basalt_calibration.json> <camA> <camB> <out_dir>
       [--template <openvins config dir>]
"""
import json, math, os, sys
import numpy as np

# OV_DYN_INIT=1 switches the whole initialisation block to dynamic init (see the comments on
# 'init_dyn_use' below). It is one coherent set of five parameters, not an independent knob each.
DYN = os.environ.get('OV_DYN_INIT', '0') not in ('0', '', 'false')


def quat_to_R(d):
    q = np.array([d['qx'], d['qy'], d['qz'], d['qw']], float)
    q /= np.linalg.norm(q)
    x, y, z, w = q
    return np.array([
        [1-2*(y*y+z*z), 2*(x*y-z*w),   2*(x*z+y*w)],
        [2*(x*y+z*w),   1-2*(x*x+z*z), 2*(y*z-x*w)],
        [2*(x*z-y*w),   2*(y*z+x*w),   1-2*(x*x+y*y)]])


def mat_yaml(M, indent="    "):
    return "\n".join(f"{indent}- [{', '.join('%.12g' % v for v in row)}]" for row in M)


def main(calib, a, b, out_dir, template_dir):
    c = json.load(open(calib))['value0']
    os.makedirs(out_dir, exist_ok=True)

    cams = []
    for idx, src in enumerate((a, b)):
        t = c['T_imu_cam'][src]
        T = np.eye(4)
        T[:3, :3] = quat_to_R(t)
        T[:3, 3] = [t['px'], t['py'], t['pz']]     # already T_imu_cam == camera -> IMU
        i = c['intrinsics'][src]['intrinsics']
        res = c['resolution'][src] if 'resolution' in c else [640, 480]
        cams.append((idx, T, i, res))

    with open(os.path.join(out_dir, 'kalibr_imucam_chain.yaml'), 'w') as f:
        f.write("%YAML:1.0\n")
        for idx, T, i, res in cams:
            other = 1 - idx
            f.write(f"cam{idx}:\n")
            f.write("  T_imu_cam: #rotation from camera to IMU R_CtoI, position of camera in IMU p_CinI\n")
            f.write(mat_yaml(T) + "\n")
            f.write(f"  cam_overlaps: [{other}]\n")
            f.write("  camera_model: pinhole\n")
            if 'k1' in i:
                f.write("  distortion_coeffs: [%.12g, %.12g, %.12g, %.12g]\n"
                        % (i['k1'], i['k2'], i['k3'], i['k4']))
                f.write("  distortion_model: equidistant\n")   # == Kannala-Brandt KB4
            else:                                              # ideal pinhole (synthetic test set)
                f.write("  distortion_coeffs: [0.0, 0.0, 0.0, 0.0]\n")
                f.write("  distortion_model: radtan\n")
            f.write("  intrinsics: [%.12g, %.12g, %.12g, %.12g]\n"
                    % (i['fx'], i['fy'], i['cx'], i['cy']))
            f.write(f"  resolution: [{res[0]}, {res[1]}]\n")
            f.write(f"  rostopic: /cam{idx}/image_raw\n")

    # IMU noise, MEASURED from the 16 s stationary segment of exports/vio-table2-2026-09-02 by
    # Allan deviation (N = sigma_ad(tau) * sqrt(tau) on the white-noise slope). The previous values
    # were carried over from a Basalt config and understated the real sensor by 21x (gyro) and
    # 4.6x (accel), which makes the filter over-confident in propagation. That was NOT what broke
    # VIO here -- the IMU frame mismatch was (notes/14) -- but over-confident noise is still wrong
    # and would bite during tuning, so these are the measured numbers.
    rate = 1000.0
    acc_n = 2.31e-03           # m/s^2/sqrt(Hz)   (was 5.06e-04)
    gyr_n = 1.85e-04           # rad/s/sqrt(Hz)   (was 8.92e-06)
    acc_w = 1.0e-04
    gyr_w = 1.0e-05
    with open(os.path.join(out_dir, 'kalibr_imu_chain.yaml'), 'w') as f:
        f.write("%YAML:1.0\nimu0:\n  T_i_b:\n" + mat_yaml(np.eye(4)) + "\n")
        f.write(f"  accelerometer_noise_density: {acc_n:.6g}\n")
        f.write(f"  accelerometer_random_walk: {acc_w:.6g}\n")
        f.write(f"  gyroscope_noise_density: {gyr_n:.6g}\n")
        f.write(f"  gyroscope_random_walk: {gyr_w:.6g}\n")
        f.write("  rostopic: /imu0\n  time_offset: 0.0\n")
        f.write(f"  update_rate: {rate}\n  model: \"calibrated\"\n")
        f.write("  Tw:\n" + mat_yaml(np.eye(3)) + "\n")
        f.write("  R_IMUtoGYRO:\n" + mat_yaml(np.eye(3)) + "\n")
        f.write("  Ta:\n" + mat_yaml(np.eye(3)) + "\n")
        f.write("  R_IMUtoACC:\n" + mat_yaml(np.eye(3)) + "\n")
        f.write("  Tg:\n" + mat_yaml(np.zeros((3, 3))) + "\n")

    # Estimator config: start from OpenVINS' EuRoC one and change only what differs for us.
    src = os.path.join(template_dir, 'estimator_config.yaml')
    lines = open(src).read().split('\n')
    repl = {
        'gravity_mag': 'gravity_mag: 9.81',
        # Dynamic init samples init_dyn_num_pose poses across this window and needs each feature seen in
        # several of them. Over 1 s the sampled poses are far enough apart that most features span
        # only ~2, leaving measurements just short of the state size. A shorter window packs the
        # poses closer together. With dynamic init actually enabled (OV_DYN_INIT=1) the opposite is
        # true -- 1 s does not observe enough parallax and the recovery is degenerate; see notes/53.
        'init_window_time': 'init_window_time: %s' % ('3.0' if DYN else '1.0'),
        'init_max_features': 'init_max_features: 100',
        # Accelerometer excitation needed to call the start of motion a "jerk". EuRoC's default
        # (1.5) and even 0.5 are tuned for a drone; a person stepping off from standing still only
        # reaches ~0.073 here, so anything higher means static init never fires and the window is
        # then lost ("platform moving too much" once disparity climbs).
        # Static init needs the OLD half of the window still and the NEW half jerking. Measured on
        # this hardware: 0.125 m/s^2 held still on the head vs 4.38 while walking -- a 35x ratio,
        # so 1.0 separates them cleanly.
        # Accelerometer check inside static init: older half must be below this, newer half above.
        # Measured 0.125 m/s^2 still on the head; the first moving window is ~0.85. 0.3 separates
        # them with margin at the onset of motion, which is where the jerk is detected.
        # 1.5 is the OpenVINS default and is ~50x this IMU's stationary accel variance (~0.03), so
        # the "old window must be stationary" guard in StaticInitializer never fires: it accepts a
        # window straddling the pickup and takes the gyro bias from it (1.75 deg/s error here, vs
        # 0.02 with 0.3). Harmless once the frame fix is in, but wrong. See notes/14.
        'init_imu_thresh': 'init_imu_thresh: %s' % os.environ.get('OV_IMU_THRESH', '0.3'),
        # OpenVINS picks static vs dynamic init by comparing image disparity against this. Our
        # captures run ~10 px, so a threshold of 10-15 classifies real motion as "stationary" and
        # forces the static path forever. Set it well below the observed disparity.
        'init_max_disparity': 'init_max_disparity: %s' % os.environ.get('OV_MAX_DISP', '10.0'),
        'calib_cam_intrinsics': 'calib_cam_intrinsics: false',   # factory calibration is trusted
        'calib_cam_extrinsics': 'calib_cam_extrinsics: false',
        # OpenVINS defaults to STATIC initialisation, which needs the device to sit still and then
        # jerk into motion. A worn headset is already moving, so static init either never fires
        # ("no accel jerk detected") or -- worse -- fires mid-motion and initialises with zero
        # velocity and gravity aligned to an accelerometer reading that includes real acceleration.
        # notes/53 CONFIRMED that prediction: it is what destroyed the room-scale walking capture,
        # which static init entered at the instant of motion onset and then dead-reckoned 3.9 km.
        # Dynamic init fixes it (11.7 cm ATE on the same data) -- but ONLY with the MLE refinement
        # below, and it REGRESSES a capture that does have a good still window, because it wins the
        # race against static init and is degenerate without excitation. So: default off, and turn
        # it on for captures with no stationary, well-textured window. See notes/53.
        'init_dyn_use': 'init_dyn_use: %s' % ('true' if DYN else 'false'),
        # Dynamic init also requires a minimum orientation change across the window; 10 deg is more
        # than a gentle look-around produces in 1 s.
        'init_dyn_min_deg': 'init_dyn_min_deg: 1.5',
        # Dynamic init solves a least-squares problem over the window; with the default 50 features
        # it ends up with fewer measurements than state parameters ("not enough feature
        # measurements: 374 meas vs 393 state size"). More features fixes that.
        # An earlier session set this to 0 ("skip the MLE") because Ceres reported "Residual and
        # Jacobian evaluation failed", and concluded the linear solution alone would bootstrap. It
        # does not: without the refinement the linear recovery is singular ("covariance recovery
        # failed" x1306, |v| coming back as 0.0001 m/s) and init never fires at all. Restoring the
        # refinement is precisely what makes dynamic init work. See notes/53.
        'init_dyn_mle_max_iter': 'init_dyn_mle_max_iter: %s' % ('50' if DYN else '0'),
        'init_dyn_mle_max_time': 'init_dyn_mle_max_time: %s' % ('1.0' if DYN else '0.05'),
        'init_dyn_num_pose': 'init_dyn_num_pose: %s' % ('8' if DYN else '6'),
        'init_dyn_min_rec_cond': 'init_dyn_min_rec_cond: %s' % ('1e-15' if DYN else '1e-12'),
    }
    out_lines = []
    for ln in lines:
        key = ln.split(':')[0].strip()
        out_lines.append(repl[key] if key in repl else ln)
    open(os.path.join(out_dir, 'estimator_config.yaml'), 'w').write('\n'.join(out_lines))
    print(f"wrote OpenVINS config for cameras ({a},{b}) to {out_dir}")


if __name__ == '__main__':
    tmpl = sys.argv[5] if len(sys.argv) > 5 else 'work/open_vins/config/euroc_mav'
    main(sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), sys.argv[4], tmpl)
