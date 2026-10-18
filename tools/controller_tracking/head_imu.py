#!/usr/bin/env python3
"""Decode and factory-rectify headset gyro, retaining per-chunk receipt times."""
import argparse
import json
from pathlib import Path
import struct
import numpy as np
from controller_imu import timed_packets


def decode_head(payload,calibration):
    if len(payload)!=36:return None
    stamp,device=struct.unpack('<II',payload[:8])
    values=np.array(struct.unpack('<7f',payload[8:]))
    if not np.isfinite(values).all():return None
    gyro=calibration['ImuCalibration']['Gyroscope']
    matrix=np.asarray(gyro['RectificationMatrix']).reshape(3,3)
    bias=np.asarray(gyro['Offset']['ConstantOffset'])
    omega=matrix@(values[3:6]*np.pi/180-bias)
    return dict(sensor_us=stamp,omega_rad_s=omega.tolist())


def main():
    ap=argparse.ArgumentParser(description=__doc__)
    for name in ('raw','index','factory_calibration','output'):ap.add_argument(name)
    args=ap.parse_args();calibration=json.loads(Path(args.factory_calibration).read_text());last=None;wrap=0;count=0
    with open(args.raw,'rb') as raw,open(args.index) as index,open(args.output,'w') as output:
        for receipt,kind,payload in timed_packets(raw,index):
            if kind!=0x50:continue
            sample=decode_head(payload,calibration)
            if sample is None:continue
            stamp=sample['sensor_us']
            if last is not None and stamp<last-(1<<31):wrap+=1
            sample['sensor_us']=stamp+(wrap<<32);last=stamp
            output.write(json.dumps(dict(receipt_mono_ns=receipt,**sample))+'\n');count+=1
    print(json.dumps(dict(samples=count)))


if __name__=='__main__':main()
