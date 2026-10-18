#!/usr/bin/env python3
"""Gyro-guided multi-camera optical prototype. Missing positions remain missing.

Consumes recordings; processing is causal after receipt-clock warmup, but live device
transport and a LineageOS runtime interface are not implemented by this replay tool.
"""
import argparse
import json
from pathlib import Path
import time
import numpy as np
from bootstrap_model import load_index,read_frame,kb4_unproject,se3
from blob_detect import find_blobs,find_led_blobs
from pose_geometry import model_reference,rotation_distance
from pnp_track import R_to_quat
from gyro_hypotheses import GyroIntegrator,relative_prediction
from gyro_optical import refine_optical
from translation_reacquisition import reacquire_translation,TranslationConfirmation
from stereo_registration import stereo_cloud,register,refine_stereo
from stereo_audit import stereo_support


def load_gyro(path,model,scale,time_correction):
    samples=[r for r in map(json.loads,Path(path).read_text().splitlines()) if r['hand']=='right']
    receipt=np.array([r['receipt_mono_ns']*1e-9 for r in samples]);sensor=np.array([r['sensor_us']*1e-6 for r in samples]);raw=np.array([r['gyro_raw'] for r in samples])
    prefix=receipt<receipt[0]+.5
    offset=np.percentile(receipt[prefix]-sensor[prefix],1)+time_correction
    G=np.array(model['gyro_rectification']).reshape(3,3);b=np.array(model['gyro_offset_si'])
    omega=(raw*scale-b)@G.T
    keep=np.r_[True,np.diff(sensor)>0]
    if np.any(np.diff(sensor[keep])<=0):raise ValueError('IMU stream reordered/reset')
    return GyroIntegrator(sensor[keep]+offset,omega[keep]),receipt[0]+.5


def load_head_gyro(path):
    samples=[json.loads(line) for line in Path(path).read_text().splitlines()]
    receipt=np.array([r['receipt_mono_ns']*1e-9 for r in samples])
    sensor=np.array([r['sensor_us']*1e-6 for r in samples])
    prefix=receipt<receipt[0]+.5
    offset=np.percentile(receipt[prefix]-sensor[prefix],1)
    omega=np.array([r['omega_rad_s'] for r in samples])
    return GyroIntegrator(sensor+offset,omega),receipt[0]+.5


def main():
    ap=argparse.ArgumentParser(description=__doc__)
    for name in ('capture','calibration','model','imu','gyro_calibration','output'):ap.add_argument(name)
    ap.add_argument('--split-bright-blobs',action='store_true');ap.add_argument('--blob-threshold',type=float,default=40);ap.add_argument('--head-imu',help='factory-rectified head gyro JSONL; experimental moving-head support');ap.add_argument('--min-peak-intensity',type=float,default=0);ap.add_argument('--cameras',type=int,nargs='+',default=[0,2],choices=range(4));ap.add_argument('--translation-reacquisition',action='store_true');ap.add_argument('--diagnostics');args=ap.parse_args();config=json.loads(Path(args.model).read_text());model,_=model_reference(config['points_imu_frame_at_capture']);normals=np.asarray(config['led_normals'])
    calibration=json.loads(Path(args.calibration).read_text())['value0'];TA,TB=[se3(calibration['T_imu_cam'][i]) for i in (0,2)]
    gc=json.loads(Path(args.gyro_calibration).read_text());gyro,warmup_until=load_gyro(args.imu,config,gc['gyro_raw_to_rad_s'],gc['time_correction_s'])
    head_gyro=None
    if args.head_imu:
        head_gyro,head_warmup=load_head_gyro(args.head_imu);warmup_until=max(warmup_until,head_warmup)
    rows=load_index(Path(args.capture)/'frames.idx');exposure=min(r['expo'] for r in rows);a=sorted([r for r in rows if r['cam']==0 and abs(r['expo']-exposure)<1e-6],key=lambda r:r['ts']);b=sorted([r for r in rows if r['cam']==2 and abs(r['expo']-exposure)<1e-6],key=lambda r:r['ts']);bt=np.array([r['ts'] for r in b])
    extras={}
    for cam in sorted(set(args.cameras)-{0,2}):
        frames=sorted([r for r in rows if r['cam']==cam and abs(r['expo']-exposure)<1e-6],key=lambda r:r['ts'])
        if frames:extras[cam]=(frames,np.array([r['ts'] for r in frames]),se3(calibration['T_imu_cam'][cam]))
    orientation=None;position=None;velocity=np.zeros(3);state_ts=None;vision_ts=None;confirmation=TranslationConfirmation()
    stats=dict(frames=0,solved=0,fast=0,reacquired=0);durations=[]
    diagnostics=open(args.diagnostics,'w') if args.diagnostics else None
    with open(Path(args.capture)/'frames.bin','rb') as blob,open(args.output,'w') as output:
        output.write('#pose_reference=centroid; inertial optical prototype; head_gyro='+str(head_gyro is not None)+'\n')
        for ra in a:
            begin=time.monotonic();ts=ra['ts']*1e-9;stats['frames']+=1;result=None;method='';views=[];view_by_camera={};detail={}
            rb=b[int(np.argmin(abs(bt-ra['ts'])))];paired=abs(rb['ts']-ra['ts'])<=100000
            selected=[(0,TA,ra)]+([(2,TB,rb)] if paired else [])
            for cam,(frames,times,T) in extras.items():
                row=frames[int(np.argmin(abs(times-ra['ts'])))];
                if abs(row['ts']-ra['ts'])<=100000:selected.append((cam,T,row))
            for cam,T,row in selected:
                image=read_frame(blob,row['off'],row['size'])
                if image is None:continue
                detector=find_led_blobs if args.split_bright_blobs else find_blobs;blobs=detector(image,thresh=args.blob_threshold);blobs=blobs[blobs[:,3]>=args.min_peak_intensity];rays=kb4_unproject(blobs[:,:2],calibration['intrinsics'][cam]['intrinsics']).reshape(-1,3)
                views.append((T,rays));view_by_camera[cam]=(T,rays)
            predicted=None
            if orientation is not None:
                delta=gyro.delta(state_ts,ts)
                head_delta=head_gyro.delta(state_ts,ts) if head_gyro is not None else None
                if delta is not None and (head_gyro is None or head_delta is not None):
                    predicted=relative_prediction(orientation,delta,head_delta)
            if predicted is not None and position is not None and ts-vision_ts<=.5:
                initial=position+velocity*min(ts-vision_ts,.12)
                if head_gyro is not None:
                    position_delta=head_gyro.delta(vision_ts,ts)
                    if position_delta is not None:initial=position_delta.T@initial
                result=refine_optical(model,normals,predicted,initial,views,diagnostics=detail)
                if result is not None and np.linalg.norm(result[1]-position)>3*(ts-vision_ts):result=None
                if result is not None:method='fast'
            if result is None and predicted is not None and args.translation_reacquisition:
                result=reacquire_translation(model,normals,predicted,views,diagnostics=detail)
                if result is not None:
                    if confirmation.observe(result[1],ts,position,vision_ts):method='reacquired'
                    else:
                        result=None
                        detail['translation_confirmation']='pending_or_motion_rejected'
                else:confirmation.reset()
            pairs=[(0,2)]+([(1,3),(0,3),(1,2),(0,1),(2,3)] if extras else [])
            for ca,cb in pairs:
                if result is not None:break
                if ts<warmup_until or ca not in view_by_camera or cb not in view_by_camera:continue
                pair_A,pair_B=view_by_camera[ca][0],view_by_camera[cb][0]
                da,db=view_by_camera[ca][1],view_by_camera[cb][1];cloud,identities=stereo_cloud(da,db,pair_A,pair_B)
                def visible(R,t,matches):
                    if predicted is not None and rotation_distance(predicted,R)>.25:return []
                    P=model@R.T+t;N=normals@R.T;mask=np.ones(len(model),bool)
                    for T in (pair_A,pair_B):
                        ray=T[:3,3]-P;mask &= np.sum(ray*N,axis=1)>=-.0872*np.linalg.norm(ray,axis=1)
                    return [pair for pair in matches if mask[pair[0]]]
                def finalize(R,t,matches):
                    matches=visible(R,t,matches)
                    if len(matches)<4:return None
                    refined=refine_stereo(model,R,t,matches,identities,da,db,pair_A,pair_B)
                    if refined is None:return None
                    R,t=refined
                    if predicted is not None and rotation_distance(predicted,R)>.25:return None
                    P=model@R.T+t;N=normals@R.T
                    sa,sb=stereo_support(P,pair_A,da,normals_imu=N),stereo_support(P,pair_B,db,normals_imu=N)
                    count=min(sa['matched'],sb['matched'])
                    if count<4:return None
                    error=(sa['mean_normalized_error']+sb['mean_normalized_error'])/2
                    score=(error+.03*rotation_distance(predicted,R),-count) if predicted is not None else (-count,error)
                    return score,R,t,error,matches
                pose,diag=register(model,cloud,identities,distance_gate=.004,finalize=finalize,match_filter=visible)
                if pose is not None:
                    R,t,error,matches=pose;result=(R,t,error,len(matches));method='reacquired'
            if result is None:
                if predicted is not None:orientation=predicted;state_ts=ts
                output.write(f"{ra['ts']},0,0,,,,,,,,,\n")
            else:
                R,t,error,count=result
                if position is not None and 0<ts-vision_ts<.12:
                    transport=head_gyro.delta(vision_ts,ts) if head_gyro is not None else np.eye(3)
                    if transport is None:transport=np.eye(3)
                    previous_position=transport.T@position
                    measured=(t-previous_position)/(ts-vision_ts)
                    velocity=.5*(transport.T@velocity)+.5*measured
                    norm=np.linalg.norm(velocity)
                    if norm>2:velocity*=2/norm
                else:velocity=np.zeros(3)
                orientation,position,state_ts,vision_ts=R,t,ts,ts
                output.write(','.join(map(str,[ra['ts'],0,count,error,method,*t,*R_to_quat(R)]))+'\n');stats['solved']+=1;stats[method]+=1
            durations.append(time.monotonic()-begin)
            if diagnostics:diagnostics.write(json.dumps(dict(ts_ns=ra['ts'],detected=[len(rays) for T,rays in views],solved=result is not None,method=method,**detail))+'\n')
            if stats['frames']%100==0:print(json.dumps(stats),flush=True)
    if diagnostics:diagnostics.close()
    stats['frame_duration_ms_percentiles']=np.percentile(durations,[50,90,99,100]).tolist();stats['frame_duration_ms_percentiles']=[v*1000 for v in stats['frame_duration_ms_percentiles']]
    print(json.dumps(stats),flush=True)


if __name__=='__main__':main()
