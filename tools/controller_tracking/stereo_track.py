#!/usr/bin/env python3
"""Experimental two-camera veto of monocular candidates; offline, no accuracy guarantee."""
import argparse
import json
from pathlib import Path
import numpy as np
from bootstrap_model import load_index, read_frame, kb4_unproject, se3
from blob_detect import find_blobs
from pose_geometry import model_reference
from pnp_track import track_frame, track_frame_prior, R_to_quat
from stereo_audit import stereo_support


def stereo_check(model, camera_a_to_imu, camera_b_to_imu, observed_b, gate=.01, minimum=4):
    def check(R, t):
        points_a = model @ R.T + t
        points_imu = points_a @ camera_a_to_imu[:3, :3].T + camera_a_to_imu[:3, 3]
        return stereo_support(points_imu, camera_b_to_imu, observed_b, gate)['matched'] >= minimum
    return check


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    for name in ('capture', 'calibration', 'model', 'output'):
        ap.add_argument(name)
    ap.add_argument('--max-blobs', type=int, default=5)
    ap.add_argument('--gate', type=float, default=.01)
    ap.add_argument('--camera-a', type=int, default=0)
    ap.add_argument('--camera-b', type=int, default=2)
    args = ap.parse_args()
    if args.max_blobs < 4 or args.gate <= 0:
        ap.error('Need at least four searched blobs and a positive gate')
    c = json.loads(Path(args.calibration).read_text())['value0']
    A, B = args.camera_a, args.camera_b
    TA, TB = se3(c['T_imu_cam'][A]), se3(c['T_imu_cam'][B])
    model, _ = model_reference(json.loads(Path(args.model).read_text())['points_imu_frame_at_capture'])
    rows = load_index(Path(args.capture) / 'frames.idx')
    exposure = min(r['expo'] for r in rows)
    short = [r for r in rows if abs(r['expo'] - exposure) < 1e-6]
    a = sorted([r for r in short if r['cam'] == A], key=lambda r:r['ts'])
    b = sorted([r for r in short if r['cam'] == B], key=lambda r:r['ts'])
    if not a or not b:
        ap.error('No short-exposure stereo frames')
    times = np.array([r['ts'] for r in b])
    previous = None
    stats = dict(frames=len(a), paired=0, solved=0, prior=0, brute=0)
    with open(Path(args.capture) / 'frames.bin', 'rb') as blob, open(args.output, 'w') as output:
        output.write('#pose_reference=centroid; experimental stereo-veto\n')
        for ra in a:
            ts = ra['ts']; rb = b[int(np.argmin(np.abs(times-ts)))]
            result = None; method = None; n = 0
            if abs(rb['ts']-ts) <= 100000:
                ia, ib = read_frame(blob,ra['off'],ra['size']), read_frame(blob,rb['off'],rb['size'])
                if ia is not None and ib is not None:
                    stats['paired'] += 1
                    ba, bb = find_blobs(ia), find_blobs(ib); n = len(ba)
                    if len(ba) >= 4 and len(bb) >= 4:
                        da = kb4_unproject(ba[:,:2],c['intrinsics'][A]['intrinsics'])
                        db = kb4_unproject(bb[:,:2],c['intrinsics'][B]['intrinsics'])
                        check = stereo_check(model,TA,TB,db,args.gate)
                        if previous is not None and 0 < (ts-previous[2])*1e-9 <= .5:
                            result = track_frame_prior(model,da,previous[0],previous[1],.03,4,.01)
                            if result is not None and not check(*result[:2]):
                                result = None
                            if result is not None: method = 'prior'
                        if result is None:
                            selected = np.argsort(-ba[:,2])[:args.max_blobs]
                            result = track_frame(model,da[selected],4,.01,candidate_check=check)
                            if result is not None: method = 'brute'
            if result is None:
                previous = None
                output.write(f'{ts},{n},0,,,,,,,,,\n')
                continue
            R,t,error = result; previous = (R,t,ts)
            Ri = TA[:3,:3] @ R; ti = TA[:3,:3] @ t + TA[:3,3]
            values = [ts,n,min(n,len(model)),error,method,*ti,*R_to_quat(Ri)]
            output.write(','.join(map(str,values))+'\n')
            stats['solved'] += 1; stats[method] += 1
    print(json.dumps(stats))


if __name__ == '__main__':
    main()
