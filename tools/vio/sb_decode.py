#!/usr/bin/env python3
# sb_decode.py — parse a /dev/syncboss_stream0 capture into Basalt/EuRoC inputs.
#   type 0x50 (len36) IMU @1kHz: {u32 ts_us, u32 id, f32 accel[3](g), f32 gyro[3](deg/s), f32 temp}
#   type 0x51 (len22) FIXED ~29.6 Hz MCU tick -- NOT camera exposure (notes/45); 0xe0 is the
#                     exposure stamp. 0x51 continues at 29.6 Hz with the cameras idle.: {u32 ts_us, ...}
# Both timestamps are the nRF 1 MHz clock (hardware-synced). Emits ns timestamps.
import sys, struct, re, math, os

G = 9.80665
DEG = math.pi/180.0

def decode(path, outdir):
    d = open(path,'rb').read()
    os.makedirs(outdir+'/imu0', exist_ok=True)
    imu = open(outdir+'/imu0/data.csv','w'); imu.write('#t[ns],wx,wy,wz,ax,ay,az\n')
    cam = open(outdir+'/cam_ts.csv','w'); cam.write('#t[ns]\n')
    n_imu=n_cam=0; wrap=0; last=None
    # sequential framing: 01 03 00 <type> 00 <len> <payload[len]>; resync on mismatch
    i=0; NL=len(d)
    while i+6 <= NL:
        if not (d[i]==1 and d[i+1]==3 and d[i+2]==0 and d[i+4]==0):
            i+=1; continue
        t=d[i+3]; L=d[i+5]; o=i
        if i+6+L > NL: break
        pl=d[i+6:i+6+L]
        i += 6+L
        if t not in (0x50,0x51): continue
        if (t==0x50 and L!=36) or (t==0x51 and L!=22): continue
        ts=struct.unpack('<I',pl[:4])[0]
        if last is not None and ts < last-(1<<28): wrap+=1
        last=ts
        tns=int((ts + (wrap<<32)) * 1000)   # us -> ns
        if t==0x50 and L==36:
            ax,ay,az,gx,gy,gz,temp = struct.unpack('<7f', pl[8:36])
            imu.write('%d,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g\n'%(tns, gx*DEG,gy*DEG,gz*DEG, ax*G,ay*G,az*G))
            n_imu+=1
        elif t==0x51 and L==22:
            cam.write('%d\n'%tns); n_cam+=1
    imu.close(); cam.close()
    print("IMU=%d (%.0f Hz) CAM=%d (%.1f Hz) -> %s"%(
        n_imu, n_imu/((last-0)/1e6+1e-9) if n_imu else 0, n_cam, 0, outdir))
    return n_imu,n_cam

if __name__=='__main__':
    decode(sys.argv[1], sys.argv[2] if len(sys.argv)>2 else 'vio_out')
