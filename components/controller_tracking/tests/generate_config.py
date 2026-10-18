#!/usr/bin/env python3
"""Generate synthetic public test calibration, never derived from a real headset."""
import json
from pathlib import Path
import sys
p=Path(sys.argv[1]);p.mkdir(parents=True,exist_ok=True)
model=[[-.04,-.03,0],[.04,-.03,.005],[.04,.03,0],[-.04,.03,.005],[0,.05,-.01],[.05,0,.01],[-.02,.01,.012]]
(p/'model.json').write_text(json.dumps(dict(points_imu_frame_at_capture=model,led_normals=[[0,0,-1]]*len(model),gyro_rectification=[1,0,0,0,1,0,0,0,1],gyro_offset_si=[0,0,0])))
(p/'gyro.json').write_text(json.dumps(dict(gyro_raw_to_rad_s=.002,time_correction_s=0)))
(p/'cameras.json').write_text(json.dumps(dict(value0=dict(T_imu_cam=[dict(px=(i-1.5)*.05,py=0,pz=0,qx=0,qy=0,qz=0,qw=1) for i in range(4)],intrinsics=[dict(intrinsics=dict(fx=300,fy=300,cx=320,cy=240,k1=0,k2=0,k3=0,k4=0)) for i in range(4)]))))
