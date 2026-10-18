#!/usr/bin/env python3
"""Offline bounded beam over stereo-supported hypotheses; no reference poses or interpolation.

Uses future frames to choose the final path. This is not a causal real-time tracker.
"""
import argparse
import json
import numpy as np
from pose_geometry import rotation_distance
from pnp_track import R_to_quat


def select_path(frames, width=32, miss_cost=4., max_gap=.5, minimum_run=3, initial_last=None):
    states=[dict(cost=0.,last=initial_last,path=[])]
    for frame in frames:
        ts=frame['ts_ns']
        candidates=[]
        for h in frame.get('hypotheses',[]):
            R=np.array(h['rotation']);t=np.array(h['position'])
            if not np.isfinite(R).all() or not np.isfinite(t).all():continue
            if any(np.linalg.norm(t-c['t'])<.002 and rotation_distance(R,c['R'])<.04 for c in candidates):continue
            candidates.append(dict(R=R,t=t,h=h))
        expanded=[]
        for candidate in candidates:
            count=-candidate['h']['score'][0]
            unary=candidate['h']['error']/.003+(8-count)*.25
            best=None
            for state in states:
                transition=0.
                if state['last'] is not None:
                    old,old_ts=state['last'];dt=(ts-old_ts)*1e-9
                    if dt<=0:continue
                    if dt<=max_gap:
                        angle=rotation_distance(old['R'],candidate['R']);distance=np.linalg.norm(old['t']-candidate['t'])
                        if angle>8*dt or distance>3*dt:continue
                        transition=.2*(angle/(4*dt))**2+.1*(distance/(.5*dt))**2
                    else:transition=2. # Explicit reacquisition; no continuity guarantee after a gap.
                cost=state['cost']+unary+transition
                if best is None or cost<best['cost']:
                    best=dict(cost=cost,last=(candidate,ts),path=state['path']+[candidate])
            if best is not None:expanded.append(best)
        for state in states:
            expanded.append(dict(cost=state['cost']+miss_cost,last=state['last'],path=state['path']+[None]))
        expanded.sort(key=lambda state:state['cost'])
        # Keep alternatives, including different last accepted poses through missing frames.
        retained=[];keys=set()
        for state in expanded:
            if state['last'] is None:key=None
            else:
                pose,stamp=state['last'];key=(stamp,tuple(np.round(pose['R'].ravel(),3)),tuple(np.round(pose['t'],4)))
            if key in keys:continue
            keys.add(key);retained.append(state)
            if len(retained)>=width:break
        states=retained
    path=min(states,key=lambda state:state['cost'])['path']
    # Confirm reacquisitions using several actual observations, never interpolated poses.
    runs=[];current=[];last_stamp=None
    for i,(frame,pose) in enumerate(zip(frames,path)):
        if pose is None:continue
        if last_stamp is not None and (frame['ts_ns']-last_stamp)*1e-9>.12:
            runs.append(current);current=[]
        current.append(i);last_stamp=frame['ts_ns']
    runs.append(current)
    for run in runs:
        if len(run)<minimum_run:
            for i in run:path[i]=None
    return path


class FixedLagTracker:
    """Causal candidate consumer with two-frame look-ahead and explicit missing results.

    Processing time is additional latency. Does not extrapolate poses or use IMU data.
    """
    def __init__(self,lag=2):
        self.lag=lag
        self.pending=[]
        self.last=None

    def push(self,frame):
        self.pending.append(frame)
        if len(self.pending)<=self.lag:return None
        minimum=2 if self.last is not None and (self.pending[0]['ts_ns']-self.last[1])*1e-9<=.5 else 3
        path=select_path(self.pending,initial_last=self.last,minimum_run=minimum)
        oldest=self.pending.pop(0);pose=path[0]
        if pose is not None:self.last=(pose,oldest['ts_ns'])
        return oldest,pose

    def finish(self):
        # No fabricated confirmation at end-of-stream: unsupported tail remains missing.
        result=[(frame,None) for frame in self.pending]
        self.pending=[]
        return result


def main():
    ap=argparse.ArgumentParser(description=__doc__);ap.add_argument('hypotheses');ap.add_argument('output');ap.add_argument('--fixed-lag',action='store_true');args=ap.parse_args()
    with open(args.hypotheses) as source:frames=[json.loads(line) for line in source]
    if args.fixed_lag:
        tracker=FixedLagTracker();emitted=[]
        for frame in frames:
            row=tracker.push(frame)
            if row is not None:emitted.append(row)
        emitted.extend(tracker.finish())
        frames=[row[0] for row in emitted];path=[row[1] for row in emitted]
    else:path=select_path(frames)
    with open(args.output,'w') as output:
        output.write('#pose_reference=centroid; stereo hypothesis beam; fixed_lag=' + str(args.fixed_lag) + '\n')
        for frame,pose in zip(frames,path):
            ts=frame['ts_ns']
            if pose is None:output.write(f'{ts},0,0,,,,,,,,,\n');continue
            h=pose['h'];values=[ts,0,int(-h['score'][0]),h['error'],'beam',*pose['t'],*R_to_quat(pose['R'])]
            output.write(','.join(map(str,values))+'\n')
    print(json.dumps(dict(frames=len(frames),solved=sum(p is not None for p in path))))


if __name__=='__main__':main()
