#!/usr/bin/env python3
"""controller_ate.py — validate pnp_track.py's poses against controller_pose_log.py's ground
truth (research-notes/57), using the same Sim3/SE3 protocol tools/vio/ate.py already established
for head-pose validation (research-notes/51) -- umeyama() is imported from there, not
reimplemented.

Two different clocks are in play, and conflating them would silently produce a meaningless number:
pnp_track.py's output is timestamped by the camera's own device clock (cts, frames.idx column 2),
which is on a different, unsynced epoch from Android's CLOCK_MONOTONIC (research-notes/51's whole
camera-timestamp-lag investigation was about exactly this). controller_pose_log's timestamps ARE
CLOCK_MONOTONIC, and so is frames.idx's OWN mono_ns column (column 3) -- captured in the same
process (the hooked trackingservice) that also produces cts, on the system-wide monotonic clock
every process shares. So: join pnp_track.py's per-frame cts back to that same frame's mono_ns via
frames.idx, and align against controller_pose_log on mono_ns -- never against cts directly.

A second problem specific to this tracker (not present in the head-pose case): a wrong blob
correspondence can pass pnp_track.py's own reprojection threshold and produce a physically
impossible frame-to-frame jump (research-notes/56 flagged this as a known, un-fixed gap; still
true after the richer capture in research-notes/58 -- more available blobs means more chances for a
plausible-looking wrong match, not fewer). Filtered here by rejecting any sample whose implied
speed from the PREVIOUS ACCEPTED sample exceeds a plausible hand-motion bound, rather than fixing
the search itself (a real fix, e.g. carrying the previous pose as a prior, is future work).

Usage: controller_ate.py <cap_dir> <pnp_poses.csv> <controller_poses.csv>
                         [--max-speed 3.0] [--max-dt 0.05]
"""
import argparse
import sys

import numpy as np

sys.path.insert(0, __file__.rsplit('/', 1)[0] + '/../vio')
from ate import umeyama


def load_frame_clock_map(cap_dir):
    """cts (device clock) -> mono_ns (CLOCK_MONOTONIC), from frames.idx columns 2 and 3."""
    m = {}
    for line in open(f'{cap_dir}/frames.idx'):
        p = line.split()
        m[int(p[2])] = int(p[3])
    return m


def load_pnp_poses(path, cts_to_mono):
    # pnp_track.py columns: ts_ns,n_blobs,n_used,reproj_err,method,px,py,pz,qx,qy,qz,qw
    rows = []
    for line in open(path):
        if line.startswith('#'):
            continue
        p = line.strip().split(',')
        if len(p) < 6 or p[5] == '':  # unsolved frame
            continue
        cts = int(p[0])
        if cts not in cts_to_mono:
            continue
        rows.append((cts_to_mono[cts], float(p[5]), float(p[6]), float(p[7])))
    rows.sort()
    return np.array(rows)  # columns: mono_ns, x, y, z


def load_ground_truth(path):
    rows = []
    for line in open(path):
        if line.startswith('#'):
            continue
        p = line.strip().split(',')
        rows.append((int(p[0]), float(p[2]), float(p[3]), float(p[4])))
    rows.sort()
    return np.array(rows)


def filter_by_speed(poses, max_speed):
    """Reject samples whose implied speed from the previous ACCEPTED sample is implausible for
    hand motion -- a wrong blob correspondence can satisfy pnp_track.py's own reprojection
    threshold and still be geometrically wrong (research-notes/56, confirmed again here)."""
    keep = [0]
    last_t, last_p = poses[0, 0], poses[0, 1:4]
    for i in range(1, len(poses)):
        t, p = poses[i, 0], poses[i, 1:4]
        dt = (t - last_t) * 1e-9
        if dt <= 0:
            continue
        speed = np.linalg.norm(p - last_p) / dt
        if speed <= max_speed:
            keep.append(i)
            last_t, last_p = t, p
    return poses[keep]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('cap_dir')
    ap.add_argument('pnp_poses')
    ap.add_argument('controller_poses')
    ap.add_argument('--max-speed', type=float, default=3.0, help='m/s, hand-motion plausibility bound')
    ap.add_argument('--max-dt', type=float, default=0.05)
    args = ap.parse_args()

    cts_to_mono = load_frame_clock_map(args.cap_dir)
    est_raw = load_pnp_poses(args.pnp_poses, cts_to_mono)
    gt = load_ground_truth(args.controller_poses)
    print(f"pnp poses (pre-filter): {len(est_raw)}   ground truth samples: {len(gt)}")

    est = filter_by_speed(est_raw, args.max_speed)
    print(f"pnp poses (post speed-filter, max {args.max_speed} m/s): {len(est)} "
          f"({len(est_raw) - len(est)} rejected)")
    if len(est) < 10:
        sys.exit("too few poses survive filtering for a meaningful alignment")

    t_gt, p_gt = gt[:, 0], gt[:, 1:4]
    j = np.clip(np.searchsorted(t_gt, est[:, 0]), 1, len(t_gt) - 1)
    j = np.where(np.abs(t_gt[j] - est[:, 0]) < np.abs(t_gt[j - 1] - est[:, 0]), j, j - 1)
    dt = np.abs(t_gt[j] - est[:, 0]) * 1e-9
    ok = dt <= args.max_dt
    print(f"matched within {args.max_dt*1e3:.0f} ms: {ok.sum()}/{len(est)} "
          f"(median {np.median(dt[ok])*1e3:.1f} ms)")
    if ok.sum() < 10:
        sys.exit("too few timestamp-matched pairs")

    pe, pg = est[ok, 1:4], p_gt[j[ok]]
    span = (est[ok, 0].max() - est[ok, 0].min()) * 1e-9
    print(f"covered span: {span:.2f} s")
    print(f"ground truth path: {np.linalg.norm(np.diff(pg, axis=0), axis=1).sum():.3f} m, "
          f"bbox {np.ptp(pg, axis=0)}")

    for name, ws in (('Sim3 (Umeyama, R+t+scale)', True), ('SE3 (scale forced to 1)', False)):
        s, R, t = umeyama(pe, pg, ws)
        al = s * (R @ pe.T).T + t
        e = np.linalg.norm(al - pg, axis=1)
        print(f"\n{name}")
        print(f"  ATE RMSE     : {np.sqrt((e**2).mean()):.4f} m")
        print(f"  median / max : {np.median(e):.4f} m / {e.max():.4f} m")
        if ws:
            print(f"  scale        : {s:.3f}")

        # The speed filter only catches a jump relative to the immediately-previous ACCEPTED
        # sample; an isolated wrong frame that happens to land near its neighbours in time (but
        # not in the true trajectory) survives it and can still dominate a global least-squares
        # fit -- Umeyama minimizes total squared error, so a handful of bad points pull the WHOLE
        # alignment (including scale) toward fitting them at everyone else's expense. Refit after
        # dropping residual outliers, same rationale as the speed filter, one level up.
        keep = e < np.median(e) + 3 * (np.median(np.abs(e - np.median(e))) + 1e-9)
        if keep.sum() < len(e) and keep.sum() >= 10:
            s2, R2, t2 = umeyama(pe[keep], pg[keep], ws)
            al2 = s2 * (R2 @ pe.T).T + t2  # re-project ALL points through the robust fit
            e2 = np.linalg.norm(al2 - pg, axis=1)
            print(f"  -- robust refit, dropped {(~keep).sum()} residual outlier(s) --")
            print(f"  ATE RMSE     : {np.sqrt((e2**2).mean()):.4f} m  (all {len(e2)} points, "
                  f"refit on the {keep.sum()} inliers)")
            print(f"  median / max : {np.median(e2):.4f} m / {e2.max():.4f} m")
            if ws:
                print(f"  scale        : {s2:.3f}")


if __name__ == '__main__':
    main()
