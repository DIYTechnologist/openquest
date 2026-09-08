#!/usr/bin/env python3
"""pnp_track.py — per-frame controller pose from the bootstrapped LED constellation model.

Two correspondence strategies, used together:

  - BRUTE-FORCE (research-notes/56): exact search over every subset/permutation of detected blobs
    against model points -- correct by construction but factorial in blob count, and has no way to
    prefer a correspondence consistent with recent motion over one that's merely locally
    low-error. research-notes/58 found real cases of both problems: 16-blob frames making an
    unmodified run take tens of minutes, and wrong-but-plausible correspondences producing either
    outright impossible frame-to-frame jumps or a systematic ~3x scale bias (ATE needed Sim3 scale
    0.342, which PnP against a metric model should never need).
  - PRIOR-GUIDED (this file, added after research-notes/58): once a pose is known, project the
    model into the image using it, match each detected blob to its nearest prediction (gated by
    distance), and refine with that pose as the initial guess. This is both far cheaper (no search)
    and self-disambiguating: a wrong correspondence has to additionally be consistent with where
    the LEDs were predicted to be, not just give a low reprojection error against the current frame
    in isolation, which is what let bad matches through before.

Brute-force still bootstraps the first frame and re-acquires whenever the prior-guided match fails
(too few gated correspondences, or reprojection error over threshold) -- the prior only helps once
tracking is already established.

Output pose is the controller-constellation-model's pose in the HEAD IMU frame (not just relative
to the one camera used) -- T_imu_model[k] = T_imu_camA @ T_camA_model[k] -- so poses are directly
comparable across frames and against Meta's own logged controller pose (research-notes/23's
pose_log/dumpsys tracking convention), since the headset itself never moves during this capture.

Usage: pnp_track.py <cap_dir> <basalt_calibration.json> <camA> <model.json> <out_poses.csv>
                    [--min-blobs 4] [--reproj-thresh 0.02] [--max-blobs 9] [--gate 0.05]
"""
import argparse
import itertools
import json
import sys

import numpy as np
import cv2

sys.path.insert(0, __file__.rsplit('/', 1)[0])
from blob_detect import find_blobs, find_static_positions, filter_static
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


def solve_pose(obj_pts, img_bearings, guess=None):
    """obj_pts (M,3) model points, img_bearings (M,3) unit bearings for a candidate correspondence.
    Treat unit bearings as normalized-pinhole image coords (x/z, y/z) at identity intrinsics, so
    cv2.solvePnP needs no distortion model -- the KB4 nonlinearity was already removed by
    kb4_unproject, same trick rectify_pair.py uses downstream of unprojection.

    guess = (R, t) seeds an iterative refinement instead of the minimal-solver SQPNP; used for the
    prior-guided path where the previous frame's pose is a good starting point and there are
    usually more than the minimal 3-4 correspondences available to refine against."""
    if len(obj_pts) < 3:
        return None
    img2d = (img_bearings[:, :2] / img_bearings[:, 2:3]).astype(np.float64)
    K = np.eye(3)
    if guess is not None:
        rvec0, _ = cv2.Rodrigues(guess[0])
        ok, rvec, tvec = cv2.solvePnP(obj_pts.astype(np.float64), img2d, K, None,
                                       rvec0.copy(), guess[1].reshape(3, 1).copy(),
                                       useExtrinsicGuess=True, flags=cv2.SOLVEPNP_ITERATIVE)
    else:
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


def track_frame_prior(model_pts, blobs_dirs, R_prev, t_prev, gate, min_blobs, reproj_thresh):
    """Predict each model point's image position from the previous pose, greedily match detected
    blobs to their nearest prediction within `gate` (normalized-bearing units), then refine.
    Returns None (falls back to brute-force re-acquisition) if too few gated matches or the
    refined error is over threshold -- this is the self-disambiguating check: a wrong
    correspondence would have to coincidentally match a MOTION-CONSISTENT prediction, not just
    look locally plausible in the current frame alone."""
    proj = (R_prev @ model_pts.T).T + t_prev
    if np.any(proj[:, 2] <= 0):
        return None  # model point predicted behind the camera; prior is stale, don't trust it
    pred2d = proj[:, :2] / proj[:, 2:3]
    img2d = blobs_dirs[:, :2] / blobs_dirs[:, 2:3]

    d = np.linalg.norm(pred2d[:, None, :] - img2d[None, :, :], axis=2)  # (n_model, n_blobs)
    matches = []
    d_work = d.copy()
    while True:
        i = np.argmin(d_work)
        mi, bi = np.unravel_index(i, d_work.shape)
        if d_work[mi, bi] > gate:
            break
        matches.append((mi, bi))
        d_work[mi, :] = np.inf
        d_work[:, bi] = np.inf
        if len(matches) == min(d.shape):
            break
    if len(matches) < min_blobs:
        return None
    obj = model_pts[[m for m, _ in matches]]
    img = blobs_dirs[[b for _, b in matches]]
    res = solve_pose(obj, img, guess=(R_prev, t_prev))
    if res is None or res[2] > reproj_thresh:
        return None
    return res


def track_frame(model_pts, blobs_dirs, min_blobs, reproj_thresh, max_k=5):
    """Brute-force correspondence search: try every way to pick and order min(len(model),
    len(blobs), max_k) of the detected blobs against that many model points, keep the lowest-error
    solve_pose result under threshold. permutations(n, k) is factorial in k as well as n --
    growing the model from 5 to 7 points (research-notes/58's static-source fix) made k=7 blow up
    the same way n=16 did before --max-blobs existed. max_k=5 is already enough points for a
    well-constrained PnP solve; this only bounds the SEARCH, not how many correspondences a solved
    frame can eventually use once the prior-guided path takes over."""
    m = len(model_pts)
    n = len(blobs_dirs)
    if n < min_blobs or m < 4:
        return None
    k = min(m, n, max_k)
    # combinations(m, k) is its own combinatorial factor on top of permutations(n, k): growing the
    # model from 5 to 7 points made this 21x bigger for free (C(7,5)=21) even with k capped, and
    # took ~24s for a single frame instead of ~1s. A handful of representative model subsets is
    # enough for a bootstrap/reacquisition solve -- this doesn't need to be exhaustive the way the
    # blob permutations (which encode the actual unknown correspondence) do.
    model_subsets = list(itertools.islice(
        itertools.combinations(range(m), k) if k < m else [tuple(range(m))], 8))
    best = None
    for blob_subset in itertools.permutations(range(n), k):
        for model_subset in model_subsets:
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
    ap.add_argument('--reproj-thresh', type=float, default=0.01,
                     help='max mean reprojection error in normalized-bearing units. Tightened from '
                          '0.02 (research-notes/59): measurably fewer wrong-correspondence outliers '
                          '(27%% -> 20%% of matched frames) at a real but smaller cost in coverage. '
                          'Tightening much further (0.006/gate 0.015) made the brute-force fallback '
                          'pathological -- almost every frame missing the prior gate and needing '
                          'reacquisition -- so this is a measured stopping point, not a proven '
                          'optimum.')
    ap.add_argument('--gate', type=float, default=0.03,
                     help='prior-guided match gate, normalized-bearing units. See --reproj-thresh.')
    ap.add_argument('--max-prior-gap-s', type=float, default=0.5,
                     help='do not trust the prior across a gap longer than this (camera clock)')
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

    # Same fix as bootstrap_model.py, same reason (research-notes/58): the largest, brightest
    # blobs in this camera are not necessarily the tracked controller's -- a completely static
    # cluster (most plausibly the other controller, sitting idle in view the whole session) can
    # outscore the real, moving one on raw area, and --max-blobs picking by area was actively
    # preferring it. Excluding anything that doesn't move is the only distinguishing signal
    # available, since the camera itself is fixed for the whole capture.
    static_A = find_static_positions(lambda r: read_frame(blob, r['off'], r['size']), ir)
    print(f"static (non-moving) sources excluded: {len(static_A)}")

    out = open(args.out_csv, 'w')
    out.write('#ts_ns,n_blobs,n_used,reproj_err,method,px,py,pz,qx,qy,qz,qw\n')
    n_solved = n_prior = n_bruteforce = 0
    prev = None  # (R_cam_model, t_cam_model, ts_ns) in camera-A frame, for the prior-guided path
    for r in ir:
        im = read_frame(blob, r['off'], r['size'])
        if im is None:
            continue
        b = filter_static(find_blobs(im), static_A)
        if len(b) == 0:
            out.write(f"{r['ts']},0,0,,,,,,,,,\n")
            prev = None
            continue
        n_detected = len(b)
        if len(b) > args.max_blobs:
            # permutations(n, k) is factorial in n for fixed k: 16 blobs (seen on richer captures,
            # research-notes/56 only ever saw <=8) makes the brute-force search 524160 candidates
            # per frame instead of 6720, turning a ~5s dataset run into an hours-long one. Keep the
            # largest-area blobs -- real LED blobs bloom brighter/bigger than most noise/reflection
            # specks -- rather than searching every detection.
            b_sorted = b[np.argsort(-b[:, 2])[:args.max_blobs]]
        else:
            b_sorted = b
        dirs_full = kb4_unproject(b[:, :2], intrA)     # for the prior path: try ALL detections,
        dirs = kb4_unproject(b_sorted[:, :2], intrA)   # cheap now that there's no search there
        method = None
        res = None
        if prev is not None and (r['ts'] - prev[2]) * 1e-9 <= args.max_prior_gap_s:
            res = track_frame_prior(model_pts, dirs_full, prev[0], prev[1], args.gate,
                                     args.min_blobs, args.reproj_thresh)
            if res is not None:
                method = 'prior'
        if res is None:
            res = track_frame(model_pts, dirs, args.min_blobs, args.reproj_thresh)
            if res is not None:
                method = 'brute'
        if res is None:
            out.write(f"{r['ts']},{n_detected},0,,,,,,,,,\n")
            prev = None
            continue
        R_cam_model, t_cam_model, err = res
        prev = (R_cam_model, t_cam_model, r['ts'])
        # camera<-model -> IMU<-model, so poses are comparable across frames / against Meta's own
        # reference (the head, which doesn't move during this capture).
        R_imu_model = TA[:3, :3] @ R_cam_model
        t_imu_model = TA[:3, :3] @ t_cam_model + TA[:3, 3]
        qx, qy, qz, qw = R_to_quat(R_imu_model)
        out.write(f"{r['ts']},{n_detected},{min(len(b),len(model_pts))},{err:.5f},{method},"
                  f"{t_imu_model[0]:.5f},{t_imu_model[1]:.5f},{t_imu_model[2]:.5f},"
                  f"{qx:.6f},{qy:.6f},{qz:.6f},{qw:.6f}\n")
        n_solved += 1
        n_prior += method == 'prior'
        n_bruteforce += method == 'brute'
        print(f"  ts={r['ts']}  blobs={n_detected}  err={err:.4f}  method={method:5s} "
              f"t=({t_imu_model[0]:+.3f},{t_imu_model[1]:+.3f},{t_imu_model[2]:+.3f})")
    out.close()
    print(f"solved {n_solved}/{len(ir)} frames ({n_prior} prior-guided, {n_bruteforce} "
          f"brute-force) -> {args.out_csv}")


if __name__ == '__main__':
    main()
