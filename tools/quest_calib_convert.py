#!/usr/bin/env python3
"""
quest_calib_convert.py — convert Quest 1 (monterey) factory calibration into
open-tracker (Basalt / Kalibr) camera-IMU configs.

Input:  the exported Meta factory calibration
        (exports/calibration-.../calibration/{camera_calibration_v2,imu_calibration}.json)
Output: three files in the chosen output dir:
        - intermediate.json         full-fidelity, loss-less (exact Meta model + transforms)
        - basalt_calibration.json   Basalt calib (KB4 approx of Fisheye62) + T_imu_cam
        - kalibr-camchain-imucam.yaml  Kalibr camchain (equidistant/KB4) + T_cam_imu

WHY AN APPROXIMATION IS INVOLVED
--------------------------------
Meta stores each tracking camera with:
  Projection = PinholeSymmetric   -> single focal f, principal point (cx, cy)
  Distortion = Fisheye62          -> 6 radial + 2 tangential coefficients
Basalt and Kalibr do NOT ingest Fisheye62. Their closest fisheye model is
Kannala-Brandt (KB4): 4 radial coefficients, radial-only.

So this tool:
  1. Computes cam<->IMU transforms EXACTLY (pure SE3 algebra) — no approximation.
  2. Passes the exact Meta intrinsics through into intermediate.json — no loss.
  3. Fits a KB4 model to the Fisheye62 radial mapping over the real field of view
     (linear least squares) and REPORTS the resulting reprojection error in pixels,
     so the approximation quality is measured, not assumed. The tiny tangential
     terms (|p| ~ 1e-4) are folded into the reported error rather than silently
     dropped.

FRAME CONVENTIONS
-----------------
Meta "AFromB" maps points B->A:  p_A = T_AfromB @ p_B.
  DeviceFromCamera[i] = T_device_cam[i]   (camera coords -> device coords)
  DeviceFromImu       = T_device_imu
Derived:
  T_imu_cam[i] = inv(DeviceFromImu) @ DeviceFromCamera[i]   (Basalt wants this)
  T_cam_imu[i] = inv(T_imu_cam[i])                          (Kalibr wants this)
"""

import argparse
import json
import os
import sys
import numpy as np


# ----------------------------------------------------------------------------- SE3 helpers
def mat4(flat16):
    m = np.array(flat16, dtype=np.float64).reshape(4, 4)
    return m


def se3_inv(T):
    R = T[:3, :3]
    t = T[:3, 3]
    Ti = np.eye(4)
    Ti[:3, :3] = R.T
    Ti[:3, 3] = -R.T @ t
    return Ti


def rot_to_quat(R):
    """Rotation matrix -> quaternion (x, y, z, w). Shepperd's method."""
    tr = np.trace(R)
    if tr > 0:
        s = np.sqrt(tr + 1.0) * 2
        w = 0.25 * s
        x = (R[2, 1] - R[1, 2]) / s
        y = (R[0, 2] - R[2, 0]) / s
        z = (R[1, 0] - R[0, 1]) / s
    elif R[0, 0] > R[1, 1] and R[0, 0] > R[2, 2]:
        s = np.sqrt(1.0 + R[0, 0] - R[1, 1] - R[2, 2]) * 2
        w = (R[2, 1] - R[1, 2]) / s
        x = 0.25 * s
        y = (R[0, 1] + R[1, 0]) / s
        z = (R[0, 2] + R[2, 0]) / s
    elif R[1, 1] > R[2, 2]:
        s = np.sqrt(1.0 + R[1, 1] - R[0, 0] - R[2, 2]) * 2
        w = (R[0, 2] - R[2, 0]) / s
        x = (R[0, 1] + R[1, 0]) / s
        y = 0.25 * s
        z = (R[1, 2] + R[2, 1]) / s
    else:
        s = np.sqrt(1.0 + R[2, 2] - R[0, 0] - R[1, 1]) * 2
        w = (R[1, 0] - R[0, 1]) / s
        x = (R[0, 2] + R[2, 0]) / s
        y = (R[1, 2] + R[2, 1]) / s
        z = 0.25 * s
    q = np.array([x, y, z, w])
    return q / np.linalg.norm(q)


def orthonormality_error(R):
    """Max abs deviation of R^T R from identity — sanity check on factory extrinsics."""
    return float(np.max(np.abs(R.T @ R - np.eye(3))))


# ------------------------------------------------------------------- Fisheye62 radial model
def fisheye62_theta_d(theta, kr):
    """
    Meta Fisheye62 radial mapping.
      theta_d = theta * (1 + kr0*th^2 + kr1*th^4 + kr2*th^6 + kr3*th^8 + kr4*th^10 + kr5*th^12)
    kr = the first 6 (radial) distortion coefficients. The 2 tangential coeffs do not
    affect the radial mapping and are handled separately (see module docstring).
    """
    th2 = theta * theta
    poly = 1.0
    p = th2
    for c in kr:
        poly = poly + c * p
        p = p * th2
    return theta * poly


def kb4_theta_d(theta, k):
    """Kannala-Brandt KB4: theta_d = theta*(1 + k0*th^2 + k1*th^4 + k2*th^6 + k3*th^8)."""
    th2 = theta * theta
    return theta * (1 + k[0] * th2 + k[1] * th2 ** 2 + k[2] * th2 ** 3 + k[3] * th2 ** 4)


def fit_kb4_to_fisheye62(f, kr, ktang, img_w, img_h, n=4000):
    """
    Fit KB4 radial coeffs to the Fisheye62 forward mapping over the true FoV.

    Returns (k[4], report) where report holds RMS/max radial reprojection error in px
    and the covered FoV. The fit is linear in KB4 coeffs:
        (theta_d/theta - 1) = k0*th^2 + k1*th^4 + k2*th^6 + k3*th^8
    Sampling is uniform in theta out to the angle whose Fisheye62 image radius reaches
    the image half-diagonal (the worst-case corner).
    """
    half_diag_px = 0.5 * np.hypot(img_w, img_h)

    # find theta_max such that f * fisheye62_theta_d(theta_max) >= half_diag_px
    th_grid = np.linspace(1e-6, 1.9, 20000)
    r_px = f * fisheye62_theta_d(th_grid, kr)
    covered = th_grid[r_px <= half_diag_px]
    theta_max = covered[-1] if covered.size else th_grid[-1]

    theta = np.linspace(1e-6, theta_max, n)
    td = fisheye62_theta_d(theta, kr)
    y = td / theta - 1.0
    A = np.stack([theta ** 2, theta ** 4, theta ** 6, theta ** 8], axis=1)
    k, *_ = np.linalg.lstsq(A, y, rcond=None)

    td_kb = kb4_theta_d(theta, k)
    radial_err_px = f * np.abs(td_kb - td)

    # Tangential magnitude at the FoV edge (reported, not fitted). Fisheye62 applies
    # tangential distortion in the *distorted* normalized coords (xr, yr), whose radius is
    # r_dist = theta_d (bounded), NOT tan(theta). At the image corner r_dist = half_diag/f,
    # so a worst-case tangential pixel displacement ~ f * |p| * r_dist^2 = |p|*half_diag^2/f.
    r_dist_corner = half_diag_px / f
    tang_px = f * np.hypot(*ktang) * (r_dist_corner ** 2)

    report = {
        "theta_max_rad": float(theta_max),
        "fov_deg_full": float(np.degrees(theta_max) * 2),
        "radial_rms_px": float(np.sqrt(np.mean(radial_err_px ** 2))),
        "radial_max_px": float(np.max(radial_err_px)),
        "tangential_worstcase_px": float(tang_px),
    }
    return [float(v) for v in k], report


# ---------------------------------------------------------------------------------- loaders
def load_calib(calib_dir):
    cam = json.load(open(os.path.join(calib_dir, "camera_calibration_v2.json")))
    imu = json.load(open(os.path.join(calib_dir, "imu_calibration.json")))
    return cam, imu


def build(cam_json, imu_json):
    serial = cam_json["Device"]["SerialNumber"]
    T_device_imu = mat4(imu_json["ImuCalibration"]["DeviceFromImu"])
    T_imu_device = se3_inv(T_device_imu)

    cams = []
    for c in sorted(cam_json["CameraCalibration"], key=lambda x: int(x["Id"])):
        proj = c["Projection"]["Coefficients"]          # [f, cx, cy]
        f, cx, cy = proj[0], proj[1], proj[2]
        dist = c["Distortion"]["Coefficients"]          # 6 radial + 2 tangential
        kr, ktang = dist[:6], dist[6:8]
        w, h = c["ImageSize"]

        T_device_cam = mat4(c["DeviceFromCamera"])
        T_imu_cam = T_imu_device @ T_device_cam
        T_cam_imu = se3_inv(T_imu_cam)

        k_kb4, fit = fit_kb4_to_fisheye62(f, kr, ktang, w, h)

        cams.append({
            "id": int(c["Id"]),
            "sensor": c["SensorType"],
            "shutter": c["Shutter"]["Type"],
            "resolution": [w, h],
            "exact": {
                "projection_model": "PinholeSymmetric",
                "f": f, "cx": cx, "cy": cy,
                "distortion_model": "Fisheye62",
                "radial": kr, "tangential": ktang,
                "DeviceFromCamera": T_device_cam.reshape(-1).tolist(),
            },
            "kb4": {
                "fx": f, "fy": f, "cx": cx, "cy": cy,
                "k1": k_kb4[0], "k2": k_kb4[1], "k3": k_kb4[2], "k4": k_kb4[3],
                "fit": fit,
            },
            "T_imu_cam": T_imu_cam,
            "T_cam_imu": T_cam_imu,
            "orthonormality_err": orthonormality_error(T_device_cam[:3, :3]),
        })
    return serial, T_device_imu, cams


# ---------------------------------------------------------------------------------- writers
def write_intermediate(path, serial, T_device_imu, imu_json, cams):
    out = {
        "_comment": "Full-fidelity, loss-less. Exact Meta Fisheye62 + exact transforms. "
                    "Use this as source of truth; the KB4 fields elsewhere are approximations.",
        "device_serial": serial,
        "imu": {
            "DeviceFromImu": T_device_imu.reshape(-1).tolist(),
            "gyroscope": imu_json["ImuCalibration"]["Gyroscope"],
            "accelerometer": imu_json["ImuCalibration"]["Accelerometer"],
        },
        "cameras": [
            {
                "id": c["id"], "sensor": c["sensor"], "shutter": c["shutter"],
                "resolution": c["resolution"], "exact": c["exact"],
                "T_imu_cam": c["T_imu_cam"].reshape(-1).tolist(),
                "kb4_approx": c["kb4"],
            } for c in cams
        ],
    }
    json.dump(out, open(path, "w"), indent=2)


def write_basalt(path, cams):
    intr, T_imu_cam, res = [], [], []
    for c in cams:
        k = c["kb4"]
        intr.append({"camera_type": "kb4", "intrinsics": {
            "fx": k["fx"], "fy": k["fy"], "cx": k["cx"], "cy": k["cy"],
            "k1": k["k1"], "k2": k["k2"], "k3": k["k3"], "k4": k["k4"]}})
        q = rot_to_quat(c["T_imu_cam"][:3, :3])
        t = c["T_imu_cam"][:3, 3]
        T_imu_cam.append({"px": t[0], "py": t[1], "pz": t[2],
                          "qx": q[0], "qy": q[1], "qz": q[2], "qw": q[3]})
        res.append(c["resolution"])
    out = {"value0": {
        "T_imu_cam": T_imu_cam,
        "intrinsics": intr,
        "resolution": res,
        "calib_accel_bias": [0.0] * 9,
        "calib_gyro_bias": [0.0] * 12,
        "_comment": "kb4 intrinsics are a fitted approximation of Meta Fisheye62; "
                    "see intermediate.json for exact model and per-camera fit error.",
    }}
    json.dump(out, open(path, "w"), indent=2)


def _yaml_mat(T):
    rows = []
    for r in range(4):
        rows.append("    - [" + ", ".join(f"{T[r, c]:.12g}" for c in range(4)) + "]")
    return "\n".join(rows)


def write_kalibr(path, cams):
    lines = ["# Kalibr camchain-imucam (generated from Quest 1 factory calibration).",
             "# distortion_model 'equidistant' == Kannala-Brandt KB4, fitted from Meta",
             "# Fisheye62; per-camera fit error in the trailing comment.",
             ""]
    for i, c in enumerate(cams):
        k = c["kb4"]
        fit = k["fit"]
        block = [f"cam{i}:"]
        if i > 0:
            T_cn = se3_inv(cams[i]["T_imu_cam"]) @ cams[i - 1]["T_imu_cam"]  # T_cn_cnm1
            block.append("  T_cn_cnm1:")
            block.append(_yaml_mat(T_cn))
        block += [
            "  T_cam_imu:",
            _yaml_mat(c["T_cam_imu"]),
            "  camera_model: pinhole",
            f"  intrinsics: [{k['fx']:.10g}, {k['fy']:.10g}, {k['cx']:.10g}, {k['cy']:.10g}]",
            "  distortion_model: equidistant",
            f"  distortion_coeffs: [{k['k1']:.10g}, {k['k2']:.10g}, {k['k3']:.10g}, {k['k4']:.10g}]",
            f"  resolution: [{c['resolution'][0]}, {c['resolution'][1]}]",
            f"  rostopic: /cam{i}/image_raw",
            f"  # sensor={c['sensor']} shutter={c['shutter']} "
            f"fov={fit['fov_deg_full']:.1f}deg "
            f"kb4_fit_rms={fit['radial_rms_px']:.4f}px max={fit['radial_max_px']:.4f}px",
        ]
        lines += block
    open(path, "w").write("\n".join(lines) + "\n")


# -------------------------------------------------------------------------------------- main
def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("calib_dir",
                    help="dir with camera_calibration_v2.json + imu_calibration.json")
    ap.add_argument("-o", "--out", default="openvr_calib_out", help="output directory")
    args = ap.parse_args()

    cam_json, imu_json = load_calib(args.calib_dir)
    serial, T_device_imu, cams = build(cam_json, imu_json)

    os.makedirs(args.out, exist_ok=True)
    write_intermediate(os.path.join(args.out, "intermediate.json"),
                       serial, T_device_imu, imu_json, cams)
    write_basalt(os.path.join(args.out, "basalt_calibration.json"), cams)
    write_kalibr(os.path.join(args.out, "kalibr-camchain-imucam.yaml"), cams)

    print(f"device: {serial}")
    print(f"cameras: {len(cams)}  |  output: {args.out}/\n")
    hdr = f"{'cam':>3} {'sensor':>8} {'res':>9} {'FoV°':>6} " \
          f"{'KB4 rms(px)':>12} {'KB4 max(px)':>12} {'tang(px)':>9} {'ortho_err':>10}"
    print(hdr); print("-" * len(hdr))
    for c in cams:
        fit = c["kb4"]["fit"]
        print(f"{c['id']:>3} {c['sensor']:>8} {str(c['resolution']):>9} "
              f"{fit['fov_deg_full']:>6.1f} {fit['radial_rms_px']:>12.4f} "
              f"{fit['radial_max_px']:>12.4f} {fit['tangential_worstcase_px']:>9.4f} "
              f"{c['orthonormality_err']:>10.2e}")
    worst = max(c["kb4"]["fit"]["radial_max_px"] for c in cams)
    print(f"\nworst-case KB4 radial reprojection error: {worst:.4f} px")
    print("exact Fisheye62 model + transforms preserved in intermediate.json")


if __name__ == "__main__":
    main()
