#!/usr/bin/env python3
"""Experimental metric stereo triangle registration; no IMU fusion or reference feedback."""
import argparse
import json
from pathlib import Path
import numpy as np
from bootstrap_model import load_index, read_frame, kb4_unproject, se3
from blob_detect import find_blobs
from pose_geometry import model_reference, rotation_distance
from pnp_track import R_to_quat
from stereo_audit import stereo_support
from stereo_registration import stereo_cloud, register, refine_stereo


def main():
    ap=argparse.ArgumentParser(description=__doc__)
    for name in ('capture','calibration','model','output'):ap.add_argument(name)
    ap.add_argument('--export-hypotheses',action='store_true',help='include camera-supported candidates in diagnostic JSONL')
    ap.add_argument('--temporal',action='store_true',help='experimental bounded-rate candidate selection')
    ap.add_argument('--start',type=int,default=0)
    ap.add_argument('--stop',type=int,default=None)
    args=ap.parse_args()
    c=json.loads(Path(args.calibration).read_text())['value0']
    model_config=json.loads(Path(args.model).read_text())
    model,_=model_reference(model_config['points_imu_frame_at_capture'])
    normals=np.asarray(model_config['led_normals']) if 'led_normals' in model_config else None
    TA,TB=se3(c['T_imu_cam'][0]),se3(c['T_imu_cam'][2])
    rows=load_index(Path(args.capture)/'frames.idx');exposure=min(r['expo'] for r in rows)
    a=sorted([r for r in rows if r['cam']==0 and abs(r['expo']-exposure)<1e-6],key=lambda r:r['ts'])
    b=sorted([r for r in rows if r['cam']==2 and abs(r['expo']-exposure)<1e-6],key=lambda r:r['ts'])
    times=np.array([r['ts'] for r in b]);stats=dict(frames=0,paired=0,solved=0,reacquired=0)
    previous=None
    with open(Path(args.capture)/'frames.bin','rb') as blob,open(args.output,'w') as output,open(args.output+'.diagnostics.jsonl','w') as diagnostic:
        output.write('#pose_reference=centroid; experimental metric stereo\n')
        for index,ra in enumerate(a):
            if index<args.start or (args.stop is not None and index>=args.stop):continue
            stats['frames']+=1
            rb=b[int(np.argmin(abs(times-ra['ts'])))];ts=ra['ts'];pose=None;diag={};n=0
            if abs(rb['ts']-ts)<=100000:
                images=[read_frame(blob,r['off'],r['size']) for r in (ra,rb)]
                if all(image is not None for image in images):
                    stats['paired']+=1
                    detections=[find_blobs(image) for image in images];n=len(detections[0])
                    da,db=[kb4_unproject(d[:,:2],c['intrinsics'][cam]['intrinsics']).reshape(-1,3) for d,cam in zip(detections,(0,2))]
                    cloud,ids=stereo_cloud(da,db,TA,TB)
                    def filter_visible(R,t,matches):
                        if normals is None:return matches
                        world=model@R.T+t;normal_world=normals@R.T
                        visible=np.ones(len(model),dtype=bool)
                        for T in (TA,TB):
                            ray=T[:3,3]-world
                            visible &= np.sum(ray*normal_world,axis=1)>=-.0872*np.linalg.norm(ray,axis=1)
                        return [pair for pair in matches if visible[pair[0]]]
                    def finalize(R,t,matches):
                        matches=filter_visible(R,t,matches)
                        if len(matches)<4:return None
                        refined=refine_stereo(model,R,t,matches,ids,da,db,TA,TB)
                        if refined is None:return None
                        R,t=refined;points=model@R.T+t
                        normal_world=normals@R.T if normals is not None else None
                        sa,sb=stereo_support(points,TA,da,normals_imu=normal_world),stereo_support(points,TB,db,normals_imu=normal_world)
                        count=min(sa['matched'],sb['matched'])
                        if count<4:return None
                        error=(sa['mean_normalized_error']+sb['mean_normalized_error'])/2
                        if args.temporal and previous is not None:
                            gap=(ts-previous[2])*1e-9
                            if 0<gap<=.25:
                                angle=rotation_distance(R,previous[0]);distance=np.linalg.norm(t-previous[1])
                                if angle>8*gap or distance>3*gap:return None
                                # Motion prior ranks only candidates already supported by both images.
                                score=(angle*.005+distance*.02+error,-count)
                            else:score=(error,-count)
                        else:score=(-count,-sa['matched']-sb['matched'],error)
                        return (score,R,t,error,matches)
                    hypotheses=[]
                    def sink(result):
                        score,R,t,error,matches=result
                        hypotheses.append(dict(rotation=R.tolist(),position=t.tolist(),error=error,score=list(score)))
                    pose,diag=register(model,cloud,ids,distance_gate=.004 if normals is not None else .008,finalize=finalize,hypothesis_sink=sink if args.export_hypotheses else None,match_filter=filter_visible)
                    if args.export_hypotheses:diag['hypotheses']=hypotheses
                    diag['stereo_cloud_points']=len(cloud)
            diagnostic.write(json.dumps(dict(index=index,ts_ns=ts,**diag))+'\n')
            if pose is None:output.write(f'{ts},{n},0,,,,,,,,,\n')
            else:
                R,t,error,matches=pose
                if previous is None or (ts-previous[2])*1e-9>.25:stats['reacquired']+=1
                previous=(R,t,ts)
                output.write(','.join(map(str,[ts,n,len(matches),error,'stereo',*t,*R_to_quat(R)]))+'\n');stats['solved']+=1
            if stats['frames']%40==0:print(json.dumps(stats),flush=True)
    print(json.dumps(stats),flush=True)


if __name__=='__main__':main()
