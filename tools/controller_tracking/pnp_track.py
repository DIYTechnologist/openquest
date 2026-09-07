#!/usr/bin/env python3
"""pnp_track.py — per-frame controller pose from the bootstrapped LED constellation model.

For each frame: detect IR blobs (blob_detect.py), unproject to bearings (same KB4 math as
bootstrap_model.py), brute-force search which subset/permutation of the model's LED points the
detected blobs correspond to (small N on both sides -- exact search, not an approximation; mirrors
the "ConstBrute"/"UnconstBrute" correspondence search trackingservice's own log field names implied
it does, research-notes/55), solve PnP for the best (lowest-reprojection-error) correspondence.

Output pose is the controller-constellation-model's pose in the HEAD IMU frame (not just relative
to the one camera used) -- T_imu_model[k] = T_imu_camA @ T_camA_model[k] -- so poses are directly
comparable across frames and against Meta's own logged controller pose (research-notes/23's
pose_log/dumpsys tracking convention), since the headset itself never moves during this capture.

Usage: pnp_track.py <cap_dir> <basalt_calibration.json> <camA> <model.json> <out_poses.csv>
                    [--min-blobs 4] [--reproj-thresh 0.02] [--max-blobs 9]
"""
import argparse
import itertools
import json
import sys

import numpy as np
import cv2

sys.path.insert(0, __file__.rsplit('/', 1)[0])
from blob_detect import find_blobs
from bootstrap_model import se3, kb4_unproject, load_index, read_frame, quat_to_R

W, H = 640, 480


def R_to_quat(R):
    t = np.trace(R)
    if t > 0:
        s = np.sqrt(t + 1.0) * 2
        w, x, y, z = 0.25*s, (R[2,1]-R[1,2])/s, (R[0,2]-R[2,0])/s, (R[1,0]-R[0,1])/s
    elif R[0,0] > R[1,1] and R[0,0] > R[2,2]:
        s = np.sqrt(1.0 + R[0,0] - R[1,1] - R[2,2]) * 2
        w, x, y, z = (R[2,1]-R[1,2])/s, 0.25*s, (R[0,1]+R[1,0])/s, (R[0,2]+R[2,0])/s
    elif R[1,1] > R[2,2]:
        s = np.sqrt(1.0 + R[1,1] - R[0,0] - R[2,2]) * 2
        w, x, y, z = (R[0,2]-R[2,0])/s, (R[0,1]+R[1,0])/s, 0.25*s, (R[1,2]+R[2,1])/s
    else:
        s = np.sqrt(1.0 + R[2,2] - R[0,0] - R[1,1]) * 2
        w, x, y, z = (R[1,0]-R[0,1])/s, (R[0,2]+R[2,0])/s, (R[1,2]+R[2,1])/s, 0.25*s
    return x, y, z, w


def solve_pose(obj_pts, img_bearings):
    """obj_pts (M,3) model points, img_bearings (M,3) unit bearings for a candidate correspondence.
    Treat unit bearings as normalized-pinhole image coords (x/z, y/z) at identity intrinsics, so
    cv2.solvePnP needs no distortion model -- the KB4 nonlinearity was already removed by
    kb4_unproject, same trick rectify_pair.py uses downstream of unprojection."""
    if len(obj_pts) < 3:
        return None
    img2d = (img_bearings[:, :2] / img_bearings[:, 2:3]).astype(np.float64)
    K = np.eye(3)
    ok, rvec, tvec = cv2.solvePnP(obj_pts.astype(np.float64), img2d, K, None,
                                   flags=cv2.SOLVEPNP_SQPNP)
    if not ok:
        return None
    R, _ = cv2.Rodrigues(rvec)
    t = tvec.ravel()
    proj = (R @ obj_pts.T).T + t
    proj2d = proj[:, :2] / proj[:, 2:3]
    err = float(np.sqrt(((proj2d - img2d) ** 2).sum(1)).mean())
    return R, t, err


def track_frame(model_pts, blobs_dirs, min_blobs, reproj_thresh):
    """Brute-force correspondence search: try every way to pick and order min(len(model),
    len(blobs)) of the detected blobs against that many model points, keep the lowest-error
    solve_pose result under threshold. Small N (<=8ish) makes this exact and still cheap."""
    m = len(model_pts)
    n = len(blobs_dirs)
    if n < min_blobs or m < 4:
        return None
    k = min(m, n)
    best = None
    for blob_subset in itertools.permutations(range(n), k):
        for model_subset in itertools.combinations(range(m), k) if k < m else [tuple(range(m))]:
            obj = model_pts[list(model_subset)]
            img = blobs_dirs[list(blob_subset)]
            res = solve_pose(obj, img)
            if res is None:
                continue
            R, t, err = res
            if err < reproj_thresh and (best is None or err < best[2]):
                best = (R, t, err)
    return best


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('cap_dir')
    ap.add_argument('calib')
    ap.add_argument('camA', type=int)
    ap.add_argument('model')
    ap.add_argument('out_csv')
    ap.add_argument('--min-blobs', type=int, default=4)
    ap.add_argument('--max-blobs', type=int, default=9,
                     help='cap on blobs searched per frame -- permutations(n,5) is factorial in n')
    ap.add_argument('--reproj-thresh', type=float, default=0.02,
                     help='max mean reprojection error in normalized-bearing units')
    args = ap.parse_args()

    c = json.load(open(args.calib))['value0']
    TA = se3(c['T_imu_cam'][args.camA])
    intrA = c['intrinsics'][args.camA]['intrinsics']

    model = json.load(open(args.model))
    model_pts = np.array(model['points_imu_frame_at_capture'])
    print(f"model: {len(model_pts)} points (from {model['source_cap_dir']})")

    rows = load_index(f'{args.cap_dir}/frames.idx')
    expos = sorted(set(round(r['expo'] * 1e6, 1) for r in rows))
    ir = sorted([r for r in rows if r['cam'] == args.camA and abs(r['expo']*1e6 - expos[0]) < 1],
                key=lambda r: r['ts'])
    print(f"{len(ir)} cam{args.camA} frames at {expos[0]} us")

    blob = open(f'{args.cap_dir}/frames.bin', 'rb')
    out = open(args.out_csv, 'w')
    out.write('#ts_ns,n_blobs,n_used,reproj_err,px,py,pz,qx,qy,qz,qw\n')
    n_solved = 0
    for r in ir:
        im = read_frame(blob, r['off'], r['size'])
        if im is None:
            continue
        b = find_blobs(im)
        if len(b) == 0:
            out.write(f"{r['ts']},0,0,,,,,,,,\n")
            continue
        n_detected = len(b)
        if len(b) > args.max_blobs:
            # permutations(n, k) is factorial in n for fixed k: 16 blobs (seen on richer captures,
            # research-notes/56 only ever saw <=8) makes the brute-force search 524160 candidates
            # per frame instead of 6720, turning a ~5s dataset run into an hours-long one. Keep the
            # largest-area blobs -- real LED blobs bloom brighter/bigger than most noise/reflection
            # specks -- rather than searching every detection.
            b = b[np.argsort(-b[:, 2])[:args.max_blobs]]
        dirs = kb4_unproject(b[:, :2], intrA)
        res = track_frame(model_pts, dirs, args.min_blobs, args.reproj_thresh)
        if res is None:
            out.write(f"{r['ts']},{n_detected},0,,,,,,,,\n")
            continue
        R_cam_model, t_cam_model, err = res
        # camera<-model -> IMU<-model, so poses are comparable across frames / against Meta's own
        # reference (the head, which doesn't move during this capture).
        R_imu_model = TA[:3, :3] @ R_cam_model
        t_imu_model = TA[:3, :3] @ t_cam_model + TA[:3, 3]
        qx, qy, qz, qw = R_to_quat(R_imu_model)
        out.write(f"{r['ts']},{n_detected},{min(len(b),len(model_pts))},{err:.5f},"
                  f"{t_imu_model[0]:.5f},{t_imu_model[1]:.5f},{t_imu_model[2]:.5f},"
                  f"{qx:.6f},{qy:.6f},{qz:.6f},{qw:.6f}\n")
        n_solved += 1
        print(f"  ts={r['ts']}  blobs={n_detected}  err={err:.4f}  "
              f"t=({t_imu_model[0]:+.3f},{t_imu_model[1]:+.3f},{t_imu_model[2]:+.3f})")
    out.close()
    print(f"solved {n_solved}/{len(ir)} frames -> {args.out_csv}")


if __name__ == '__main__':
    main()
