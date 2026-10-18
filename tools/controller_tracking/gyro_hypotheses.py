#!/usr/bin/env python3
"""Experimental gyro-guided stereo selection for a physically fixed headset.

Calibration maps raw gyro into controller axes, and controller axes into model axes.
No reference trajectory is read during selection. Pose gaps remain missing; gyro alone
is never used to invent a 6DoF position. Optional head-gyro compensation remains experimental.
"""
import argparse
import json
from pathlib import Path
import cv2
import numpy as np
from pnp_track import R_to_quat
from pose_geometry import rotation_distance


class GyroIntegrator:
    def __init__(self,times,omega):
        self.times=np.asarray(times);self.omega=np.asarray(omega)
        if self.times.ndim!=1 or self.omega.shape!=(len(self.times),3):raise ValueError('Invalid IMU shape')
        if len(self.times)<2 or np.any(np.diff(self.times)<=0):raise ValueError('IMU clock is not strictly increasing')
        if not np.isfinite(self.times).all() or not np.isfinite(self.omega).all():raise ValueError('Nonfinite IMU data')

    def delta(self,start,end):
        if end<=start or start<self.times[0] or end>self.times[-1]:return None
        inside=self.times[(self.times>start)&(self.times<end)]
        grid=np.r_[start,inside,end]
        if np.max(np.diff(grid))>.01:return None
        values=np.column_stack([np.interp(grid,self.times,self.omega[:,j]) for j in range(3)])
        result=np.eye(3)
        for i,dt in enumerate(np.diff(grid)):
            result=result@cv2.Rodrigues((values[i]+values[i+1])*.5*dt)[0]
        return result


def relative_prediction(previous,controller_delta,head_delta=None):
    """Predict model-to-head orientation using both independently measured body rotations."""
    predicted=previous@controller_delta
    return head_delta.T@predicted if head_delta is not None else predicted


def select_gyro(frames,gyro,visual_gain=.15,head_gyro=None):
    last=None;result=[]
    for frame in frames:
        ts=frame['ts_ns']*1e-9;candidates=frame.get('hypotheses',[])
        best=None;predicted=None
        if last is not None and 0<ts-last[2]<=.5:
            delta=gyro.delta(last[2],ts)
            if delta is not None:
                head_delta=head_gyro.delta(last[2],ts) if head_gyro is not None else None
                if head_gyro is None or head_delta is not None:
                    predicted=relative_prediction(last[0],delta,head_delta)
        for h in candidates:
            R=np.asarray(h['rotation']);t=np.asarray(h['position']);count=-h['score'][0]
            if predicted is None:
                if count<6:continue # Conservative optical initialization.
                score=tuple(h['score'])
            else:
                angle=rotation_distance(predicted,R)
                if angle>.2 or np.linalg.norm(t-last[1])>3*(ts-last[2]):continue
                score=(h['error']+.03*angle,-count)
            if best is None or score<best[0]:best=(score,R,t,h)
        if best is None:result.append(None);continue
        _,R,t,h=best
        if predicted is not None:
            correction=cv2.Rodrigues(predicted.T@R)[0].ravel()
            R=predicted@cv2.Rodrigues(visual_gain*correction)[0]
        last=(R,t,ts);result.append(dict(R=R,t=t,h=h))
    return result


def main():
    ap=argparse.ArgumentParser(description=__doc__)
    for name in ('hypotheses','imu','gyro_calibration','axis_calibration','output'):ap.add_argument(name)
    ap.add_argument('--head-imu',help='optional factory-rectified head gyro JSONL; head-motion path not hardware-validated');args=ap.parse_args()
    frames=[json.loads(line) for line in Path(args.hypotheses).read_text().splitlines()]
    imu=[row for row in map(json.loads,Path(args.imu).read_text().splitlines()) if row['hand']=='right']
    calibration=json.loads(Path(args.gyro_calibration).read_text());axis=json.loads(Path(args.axis_calibration).read_text())
    receipt=np.array([row['receipt_mono_ns']*1e-9 for row in imu]);sensor=np.array([row['sensor_us']*1e-6 for row in imu])
    # Each capture needs a fresh receipt-to-sensor offset. This does not calibrate variable transport latency.
    offset=np.percentile(receipt-sensor,1)+calibration['time_correction_s']
    Q=np.asarray(calibration['rotation_row_mapping']);B=np.asarray(axis['model_from_controller_rotation'])
    raw=np.array([row['gyro_raw'] for row in imu]);omega=(raw@Q*calibration['gyro_raw_to_rad_s']+calibration['bias_rad_s'])@B.T
    # Packet duplicates may be retransmitted; retain one sample for each sensor timestamp.
    unique=np.zeros(len(sensor),dtype=bool);last_stamp=-float('inf')
    for i,stamp in enumerate(sensor):
        if stamp>last_stamp:unique[i]=True;last_stamp=stamp
    gyro=GyroIntegrator(sensor[unique]+offset,omega[unique])
    head_gyro=None
    if args.head_imu:
        samples=[json.loads(line) for line in Path(args.head_imu).read_text().splitlines()]
        ht=np.array([row['sensor_us']*1e-6 for row in samples]);hr=np.array([row['receipt_mono_ns']*1e-9 for row in samples]);ho=np.array([row['omega_rad_s'] for row in samples])
        head_gyro=GyroIntegrator(ht+np.percentile(hr-ht,1),ho)
    path=select_gyro(frames,gyro,head_gyro=head_gyro)
    with open(args.output,'w') as output:
        output.write('#pose_reference=centroid; experimental gyro-guided stereo; head_compensation=' + str(head_gyro is not None) + '\n')
        for frame,pose in zip(frames,path):
            ts=frame['ts_ns']
            if pose is None:output.write(f'{ts},0,0,,,,,,,,,\n');continue
            h=pose['h'];values=[ts,0,int(-h['score'][0]),h['error'],'gyro',*pose['t'],*R_to_quat(pose['R'])]
            output.write(','.join(map(str,values))+'\n')
    print(json.dumps(dict(frames=len(frames),solved=sum(p is not None for p in path))))


if __name__=='__main__':main()
