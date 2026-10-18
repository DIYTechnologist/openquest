#!/usr/bin/env python3
"""Replay pnp_track --observations-jsonl without raw camera images or reference poses."""
import argparse
import json
import numpy as np

from pnp_track import track_observation, R_to_quat


def replay(source, output, reject_ambiguous=None):
    config = json.loads(next(source))
    if config.get('type') != 'config' or config.get('schema') != 1:
        raise ValueError('Expected a schema-1 observation configuration')
    model = np.asarray(config['model_points'], dtype=float)
    transform = np.asarray(config['camera_to_imu'], dtype=float)
    if model.ndim != 2 or model.shape[1] != 3 or transform.shape != (4, 4):
        raise ValueError('Invalid model or camera transform shape')
    if not np.isfinite(model).all() or not np.isfinite(transform).all():
        raise ValueError('Non-finite replay calibration')
    parameters = dict(config.get('tracker_parameters', {}))
    allowed = {'gate', 'min_blobs', 'reproj_thresh', 'max_prior_gap_s', 'max_speed', 'reject_ambiguous'}
    if set(parameters) - allowed:
        raise ValueError('Unknown replay tracker parameter')
    if reject_ambiguous is not None:
        parameters['reject_ambiguous'] = reject_ambiguous
    previous = None
    stats = {'frames': 0, 'solved': 0, 'ambiguous': 0, 'rejected_ambiguous': 0}
    for line in source:
        frame = json.loads(line)
        if frame.get('type') != 'frame':
            raise ValueError('Expected an observation frame')
        full = np.asarray(frame['bearings'], dtype=float).reshape(-1, 3)
        searched = np.asarray(frame['searched_bearings'], dtype=float).reshape(-1, 3)
        timestamp = int(frame['ts_ns'])
        pose, method, diagnostic = track_observation(model, full, searched, timestamp, previous,
                                                     **parameters)
        row = {'ts_ns': timestamp, 'reference': config['reference'], 'method': method,
               'diagnostic': diagnostic, 'pose': None}
        stats['frames'] += 1
        stats['ambiguous'] += bool(diagnostic.get('ambiguous'))
        stats['rejected_ambiguous'] += bool(diagnostic.get('rejected_for_ambiguity'))
        if pose is None:
            previous = None
        else:
            R, t, error = pose
            previous = (R, t, timestamp)
            R_imu = transform[:3, :3] @ R
            p_imu = transform[:3, :3] @ t + transform[:3, 3]
            row['pose'] = {'position': p_imu.tolist(), 'quaternion_xyzw': list(R_to_quat(R_imu)),
                           'mean_normalized_reprojection_error': error}
            stats['solved'] += 1
        output.write(json.dumps(row) + '\n')
    return stats


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('observations')
    parser.add_argument('output')
    parser.add_argument('--reject-ambiguous', action='store_true', default=None)
    args = parser.parse_args()
    with open(args.observations) as source, open(args.output, 'w') as output:
        print(json.dumps(replay(source, output, args.reject_ambiguous)))


if __name__ == '__main__':
    main()
