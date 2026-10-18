#!/usr/bin/env python3
"""Decode controller IMU records with host receipt times; gyro units deliberately remain raw."""
import argparse
from collections import deque
import csv
import json
import struct
import sys
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parents[2]/'components/controllers'))
from ctl_decode import REC_SIZE,handedness


def imu_records(payload):
    if len(payload)<23:return
    hand=handedness(payload)
    if hand is None:return
    j=23
    while j+2<=len(payload):
        tag,marker=payload[j:j+2]
        if tag==0x41 and marker==0x82:
            if j+20>len(payload):return
            record=payload[j+2:j+20]
            yield dict(hand=hand,sensor_us=int.from_bytes(record[:6],'little'),
                       accel_raw=list(struct.unpack('<3h',record[6:12])),
                       gyro_raw=list(struct.unpack('<3h',record[12:18])))
            j+=20
        elif marker==0x80 and tag in REC_SIZE:
            size=2+REC_SIZE[tag]
            if j+size>len(payload):return
            j+=size
        else:return


def timed_packets(raw,index):
    """Frame raw chunks while retaining the receipt time of the final chunk of each packet."""
    pending=bytearray();offset=0
    for row in csv.reader(line for line in index if not line.startswith('#')):
        timestamp,start,length=map(int,row)
        if start!=offset or length<0 or length>65536:raise ValueError('Invalid/noncontiguous receipt index')
        chunk=raw.read(length)
        if len(chunk)!=length:raise ValueError('Truncated raw recording')
        offset+=length;pending.extend(chunk);i=0
        while i+6<=len(pending):
            if pending[i:i+3]!=b'\x01\x03\x00' or pending[i+4]!=0:i+=1;continue
            kind,length=pending[i+3],pending[i+5];end=i+6+length
            if end>len(pending):break
            yield timestamp,kind,bytes(pending[i+6:end]);i=end
        del pending[:i]


def main():
    ap=argparse.ArgumentParser(description=__doc__)
    for name in ('raw','index','output'):ap.add_argument(name)
    args=ap.parse_args();recent={};count=0
    with open(args.raw,'rb') as raw,open(args.index) as index,open(args.output,'w') as output:
        for receipt,kind,payload in timed_packets(raw,index):
            if kind!=0x8f:continue
            device=payload[:8]
            for sample in imu_records(payload):
                queue,seen=recent.setdefault(device,(deque(),set()))
                stamp=sample['sensor_us']
                if stamp in seen:continue
                queue.append(stamp);seen.add(stamp)
                if len(queue)>64:seen.remove(queue.popleft())
                output.write(json.dumps(dict(receipt_mono_ns=receipt,**sample))+'\n');count+=1
    print(json.dumps(dict(samples=count)))


if __name__=='__main__':main()
