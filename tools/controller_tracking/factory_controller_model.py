#!/usr/bin/env python3
"""Import a local Quest 1 controllercal file. No serials or raw calibration are bundled.

Observed format: little-endian uint64 JSON byte length followed by that JSON.
The legacy model-point key is retained for compatibility; coordinates are factory model axes.
"""
import argparse
import json
from pathlib import Path
import struct
import numpy as np


def import_model(data):
    if len(data)<8:raise ValueError('Truncated controller calibration header')
    length=struct.unpack_from('<Q',data)[0]
    if length!=len(data)-8 or length>1_000_000:raise ValueError('Invalid controller calibration JSON length')
    root=json.loads(data[8:]);tracked=root['TrackedObject'];entries=tracked['ModelPoints']
    names=sorted(entries,key=lambda key:int(key.removeprefix('Point')))
    if len(names)<4:raise ValueError('Need at least four factory LEDs')
    points=[];normals=[]
    for key in names:
        record=np.asarray(entries[key],dtype=float)
        if record.ndim!=1 or len(record)<6 or not np.isfinite(record).all():raise ValueError('Invalid LED calibration entry')
        point,normal=record[:3],record[3:6]
        if np.linalg.norm(point)>.5 or not .9<np.linalg.norm(normal)<1.1:raise ValueError('Implausible LED point or normal')
        points.append(point.tolist());normals.append((normal/np.linalg.norm(normal)).tolist())
    imu=np.asarray(tracked['ImuPosition'],dtype=float)
    gyro=np.asarray(tracked['GyroCalibration'],dtype=float)
    if imu.shape!=(3,) or gyro.shape!=(12,) or not np.isfinite(imu).all() or not np.isfinite(gyro).all():raise ValueError('Invalid IMU geometry/calibration')
    rectification=np.asarray(root.get('gyro_m',gyro[:9]),dtype=float)
    bias=np.asarray(root.get('gyro_b',gyro[9:]),dtype=float)
    if rectification.shape!=(9,) or bias.shape!=(3,) or not np.isfinite(rectification).all() or not np.isfinite(bias).all():raise ValueError('Invalid rectified gyro calibration')
    return dict(gyro_rectification=rectification.tolist(),gyro_offset_si=bias.tolist(),source_cap_dir='local factory controller calibration',coordinate_frame='factory tracked-object model axes',
                points_imu_frame_at_capture=points,led_normals=normals,led_ids=[int(key.removeprefix('Point')) for key in names],
                imu_position_model=imu.tolist(),gyro_calibration=gyro.tolist(),factory_json_version=tracked.get('JsonVersion'))


def main():
    ap=argparse.ArgumentParser(description=__doc__);ap.add_argument('controllercal');ap.add_argument('output');args=ap.parse_args()
    model=import_model(Path(args.controllercal).read_bytes());Path(args.output).write_text(json.dumps(model,indent=2)+'\n')
    print(json.dumps(dict(leds=len(model['led_ids']),has_normals=True,has_imu_position=True)))


if __name__=='__main__':main()
