#!/usr/bin/env python3
"""Audit centroid PnP poses against a second camera; diagnostic, not ground truth.

Uses greedy unique bearing matches. Missing/occluded LEDs can fail this check;
a pass does not establish identity or absolute accuracy. No poses are modified.
"""
import argparse
import csv
import json
from pathlib import Path
import numpy as np
from bootstrap_model import load_index, read_frame, kb4_unproject, se3, quat_to_R
from blob_detect import find_blobs
from pose_geometry import model_reference


def stereo_support(points_imu, camera_to_imu, observed, gate=.01, normals_imu=None):
    camera = (points_imu - camera_to_imu[:3, 3]) @ camera_to_imu[:3, :3]
    mask=camera[:,2]>1e-9
    if normals_imu is not None:
        to_camera=camera_to_imu[:3,3]-points_imu
        cosine=np.sum(normals_imu*to_camera,axis=1)/np.maximum(np.linalg.norm(to_camera,axis=1),1e-12)
        mask &= cosine>=-.0872 # 95 degree conservative emission cone, including boundary margin.
    visible = camera[mask]
    if len(visible) == 0 or len(observed) == 0:
        return {'matched': 0, 'predicted_front': len(visible), 'observed': len(observed),
                'mean_normalized_error': None}
    predicted = visible[:, :2] / visible[:, 2:3]
    observed = np.asarray(observed)
    observed = observed[observed[:, 2] > 1e-9]
    distance = np.linalg.norm(predicted[:, None] - (observed[:, :2] / observed[:, 2:3])[None], axis=2)
    errors = []
    # Greedy assignment is deliberately a diagnostic, not an exhaustive matching test.
    for index in np.argsort(distance, axis=None):
        i, j = np.unravel_index(index, distance.shape)
        error = distance[i, j]
        if error > gate:
            continue
        errors.append(float(error))
        distance[i, :] = np.inf
        distance[:, j] = np.inf
    return {'matched': len(errors), 'predicted_front': len(visible), 'observed': len(observed),
            'mean_normalized_error': float(np.mean(errors)) if errors else None}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('capture', 'calibration', 'model', 'poses', 'output'):
        parser.add_argument(name)
    parser.add_argument('--camera', type=int, default=2)
    parser.add_argument('--gate', type=float, default=.01)
    parser.add_argument('--max-lag-ns', type=int, default=100000)
    args = parser.parse_args()
    if '#pose_reference=centroid;' not in Path(args.poses).read_text():
        parser.error('Requires centroid-reference pose CSV')
    calibration = json.loads(Path(args.calibration).read_text())['value0']
    transform = se3(calibration['T_imu_cam'][args.camera])
    intrinsics = calibration['intrinsics'][args.camera]['intrinsics']
    model, _ = model_reference(json.loads(Path(args.model).read_text())['points_imu_frame_at_capture'])
    rows = [r for r in load_index(Path(args.capture) / 'frames.idx') if r['cam'] == args.camera]
    exposure = min(r['expo'] for r in rows)
    rows = sorted([r for r in rows if abs(r['expo'] - exposure) < 1e-6], key=lambda r: r['ts'])
    times = np.array([r['ts'] for r in rows])
    stats = {'solved_input': 0, 'paired': 0, 'support_at_least_four': 0}
    with open(Path(args.capture) / 'frames.bin', 'rb') as blob, open(args.poses) as poses, open(args.output, 'w') as output:
        for p in csv.reader(line for line in poses if not line.startswith('#')):
            if not p[5]:
                continue
            stats['solved_input'] += 1
            ts = int(p[0]); row = rows[int(np.argmin(np.abs(times - ts)))]
            if abs(row['ts'] - ts) > args.max_lag_ns:
                continue
            image = read_frame(blob, row['off'], row['size'])
            if image is None:
                continue
            b = find_blobs(image)
            observed = kb4_unproject(b[:, :2], intrinsics).reshape(-1, 3)
            q = dict(zip(('qx', 'qy', 'qz', 'qw'), map(float, p[8:12])))
            points = model @ quat_to_R(q).T + np.array(list(map(float, p[5:8])))
            support = stereo_support(points, transform, observed, args.gate)
            stats['paired'] += 1
            stats['support_at_least_four'] += support['matched'] >= 4
            output.write(json.dumps(dict(ts_ns=ts, pair_delta_ns=row['ts']-ts, **support)) + '\n')
    print(json.dumps(stats))


if __name__ == '__main__':
    main()
