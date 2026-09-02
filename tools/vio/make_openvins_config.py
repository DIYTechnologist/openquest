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
            f.write("  distortion_coeffs: [%.12g, %.12g, %.12g, %.12g]\n"
                    % (i['k1'], i['k2'], i['k3'], i['k4']))
            f.write("  distortion_model: equidistant\n")   # == Kannala-Brandt KB4
            f.write("  intrinsics: [%.12g, %.12g, %.12g, %.12g]\n"
                    % (i['fx'], i['fy'], i['cx'], i['cy']))
            f.write(f"  resolution: [{res[0]}, {res[1]}]\n")
            f.write(f"  rostopic: /cam{idx}/image_raw\n")

    # IMU: ICM-20602 at ~1 kHz. Noise densities taken from the Basalt calibration, converted from
    # per-sample sigma to continuous-time density (sigma * sqrt(dt)).
    rate = 1000.0
    acc_n = 0.016 / math.sqrt(rate)
    gyr_n = 0.000282 / math.sqrt(rate)
    acc_w = 0.001 * math.sqrt(rate) / rate
    gyr_w = 0.0001 * math.sqrt(rate) / rate
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
        'init_window_time': 'init_window_time: 1.0',
        'init_imu_thresh': 'init_imu_thresh: 0.5',      # our capture never sits fully still
        'init_max_disparity': 'init_max_disparity: 15.0',
        'calib_cam_intrinsics': 'calib_cam_intrinsics: false',   # factory calibration is trusted
        'calib_cam_extrinsics': 'calib_cam_extrinsics: false',
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
