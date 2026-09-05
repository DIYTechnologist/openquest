#!/usr/bin/env python3
"""build_euroc_leech.py — EuRoC dataset from an ibfs_hook9 leech capture.

Input is the hook9 pair: a single frame blob plus a text index, one row per emitted frame:

    fno cam capture_ns host_ns offset size entry_id block_index exposure gain

Three things this has to get right, all of them learned the hard way:

1. **Deduplicate by (cam, capture_ns).** The hook's emit path races across delivery threads, so the
   same frame can be written more than once (notes/40). The capture timestamp is authoritative and
   changes exactly once per frame, so it is the key.

2. **Use ONE exposure parity.** The camera delivers two interleaved 25 Hz streams at different
   exposures, 40 ms apart, offset ~17.8 ms (notes/41). Consecutive frames differ in exposure by 22x
   more than same-parity frames. Feeding a VIO both would flip image brightness every frame — and
   it would show up only as a poor ATE, which is near-impossible to diagnose after the fact.
   Parity 1 is the brighter, scene-adapting stream (exp*gain 0.099 vs 0.053) and is the default.

3. **Give a stereo pair identical timestamps.** EuRoC pairs frames by exact timestamp match, so the
   two cameras' capture times (which differ by a few ms) are unified onto cam A's value after
   nearest-neighbour pairing.

IMU comes from the sb_leech raw stream: type 0x50, {u32 ts_us (1 MHz nRF clock), u32 id,
f32 accel[3] g, f32 gyro[3] deg/s, f32 temp}, converted to EuRoC's rad/s and m/s^2.

**IMU RECTIFICATION IS MANDATORY** (notes/14, and re-learned the hard way here). The syncboss FIFO
delivers RAW sensor-frame samples. The factory calibration carries a per-sensor RectificationMatrix
mapping raw axes into the IMU *body* frame, and it is very nearly a 180 deg rotation: body ~
(-y, -x, -z) of raw. Every camera->IMU extrinsic is expressed in that body frame, so feeding raw
samples puts the IMU and cameras ~180 deg apart. Nothing static catches it -- gyro and accel stay
mutually consistent, the accelerometer still reads a clean 1 g at rest -- but every visual update
becomes inconsistent with propagation, triangulation fails, chi2 rejects the survivors and the
filter silently dead-reckons. Measured previously: 101 deg median axis error raw vs 9 deg rectified,
and it accounted for ~600 m of drift. Convention is Rect @ (raw - Offset).

NOTE ON CLOCKS: frame stamps are CLOCK_MONOTONIC (from the FrameSet descriptor) while IMU stamps
are the nRF 1 MHz clock. They are NOT the same timebase. The offset is recovered by correlation
(notes/31 measured r=0.997 at ~0.07 s) and applied with --imu-offset-ns; without it the dataset is
internally inconsistent and VIO will not converge.

Usage: build_euroc_leech.py <frames.bin> <frames.idx> <imu.bin> <out_dir> [--parity 1]
                            [--camA 0] [--camB 2] [--imu-offset-ns N] [--pair-tol-ms 8]
"""
import argparse
import json
import math
import os
import struct
import sys

import numpy as np
from PIL import Image

W, H = 640, 481          # buffer height; row 0 is metadata, see CROP_TOP
CROP_TOP = 1             # the first row is a metadata line, not image data
G = 9.80665
DEG = math.pi / 180.0


def load_index(path):
    rows = []
    for ln in open(path):
        p = ln.split()
        if len(p) < 10:
            continue
        rows.append(dict(cam=int(p[1]), ts=int(p[2]), off=int(p[4]), size=int(p[5]),
                         expo=float(p[8]), gain=float(p[9])))
    seen = set()
    uniq = []
    for r in rows:                      # dedupe: emit path races, capture ts is authoritative
        key = (r['cam'], r['ts'])
        if key in seen:
            continue
        seen.add(key)
        uniq.append(r)
    return rows, uniq


def load_rect(calib_path):
    """-> (R_gyro, off_gyro, R_accel, off_accel) from the factory intermediate.json."""
    c = json.load(open(calib_path))['imu']
    return (np.array(c['gyroscope']['RectificationMatrix'], float).reshape(3, 3),
            np.array(c['gyroscope']['Offset']['ConstantOffset'], float),
            np.array(c['accelerometer']['RectificationMatrix'], float).reshape(3, 3),
            np.array(c['accelerometer']['Offset']['OffsetAtZeroDegC'], float))


def load_imu(path, offset_ns, rect=None):
    d = open(path, 'rb').read()
    i, out = 0, []
    while i + 6 <= len(d):
        if not (d[i] == 1 and d[i+1] == 3 and d[i+2] == 0 and d[i+4] == 0):
            i += 1
            continue
        t, L = d[i+3], d[i+5]
        if i + 6 + L > len(d):
            break
        if t == 0x50 and L == 36:
            ts, _ = struct.unpack('<II', d[i+6:i+14])
            ax, ay, az, gx, gy, gz, _tp = struct.unpack('<7f', d[i+14:i+42])
            # UNITS: the stream is deg/s and g; the calibration Offsets are SI (rad/s and m/s^2).
            # Convert to SI FIRST, then Rect @ (x - Offset). Measured on the still window of the
            # 2026-09-05 capture, this is not a cosmetic choice:
            #   SI-first          |gyro| 0.00772 rad/s   <- correct
            #   offset in deg/s   |gyro| 0.06105         <- barely better than no offset
            #   no offset         |gyro| 0.06214
            gv = np.array([gx, gy, gz]) * DEG
            av = np.array([ax, ay, az]) * G
            if rect is not None:
                Rg, og, Ra, oa = rect
                gv = Rg @ (gv - og)
                av = Ra @ (av - oa)
            out.append((ts * 1000 + offset_ns, gv[0], gv[1], gv[2], av[0], av[1], av[2]))
        i += 6 + L
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('blob'); ap.add_argument('index'); ap.add_argument('imu'); ap.add_argument('out')
    ap.add_argument('--parity', type=int, default=1)
    ap.add_argument('--camA', type=int, default=0)
    ap.add_argument('--camB', type=int, default=2)
    ap.add_argument('--imu-offset-ns', type=int, default=0)
    ap.add_argument('--pair-tol-ms', type=float, default=8.0)
    ap.add_argument('--calib', default='exports/calibration-2026-08-30/openvr_calib_out/intermediate.json',
                    help='factory intermediate.json for IMU rectification; --calib none disables')
    a = ap.parse_args()

    rows, uniq = load_index(a.index)
    print(f"index rows {len(rows)} -> unique (cam,ts) {len(uniq)}")

    sel = {}
    for cam in (a.camA, a.camB):
        t = sorted([r for r in uniq if r['cam'] == cam], key=lambda r: r['ts'])
        sel[cam] = t[a.parity::2]
        ex = np.array([r['expo'] for r in sel[cam]])
        print(f"cam{cam}: {len(t)} unique -> parity{a.parity} {len(sel[cam])} frames, "
              f"exposure {ex.mean():.5f} +/- {ex.std():.5f}")

    tb = np.array([r['ts'] for r in sel[a.camB]])
    tol = a.pair_tol_ms * 1e6
    pairs = []
    for r in sel[a.camA]:
        j = int(np.argmin(np.abs(tb - r['ts'])))
        if abs(tb[j] - r['ts']) <= tol:
            pairs.append((r, sel[a.camB][j]))
    print(f"stereo pairs within {a.pair_tol_ms} ms: {len(pairs)} / {len(sel[a.camA])}")
    if not pairs:
        sys.exit("no stereo pairs formed")

    for sub in ('cam0/data', 'cam1/data', 'imu0'):
        os.makedirs(os.path.join(a.out, 'mav0', sub), exist_ok=True)
    blob = open(a.blob, 'rb')
    csv0 = open(os.path.join(a.out, 'mav0/cam0/data.csv'), 'w')
    csv1 = open(os.path.join(a.out, 'mav0/cam1/data.csv'), 'w')
    for c in (csv0, csv1):
        c.write('#timestamp [ns],filename\n')
    t0 = pairs[0][0]['ts']
    for ra, rb in pairs:
        ts = ra['ts']                    # both cameras take cam A's stamp: EuRoC needs exact match
        for r, sub, csv in ((ra, 'cam0', csv0), (rb, 'cam1', csv1)):
            blob.seek(r['off'])
            buf = blob.read(r['size'])
            if len(buf) != r['size']:
                continue
            # Drop row 0: it is a metadata line, not pixels. Measured on a real frame, row 0 has
            # mean 4.45 and differs from row 1 by 83 grey levels, where every other adjacent row
            # pair differs by ~6. Keeping it would both feed the tracker a garbage scanline and
            # shift the principal point by a pixel relative to the factory calibration, which is
            # specified for 640x480.
            img = Image.frombytes('L', (W, H), buf).crop((0, CROP_TOP, W, H))
            img.save(os.path.join(a.out, 'mav0', sub, 'data', f'{ts}.png'))
            csv.write(f'{ts},{ts}.png\n')
    csv0.close(); csv1.close()

    rect = None if a.calib == 'none' else load_rect(a.calib)
    print('IMU rectification: ' + ('ON  (Rect @ (raw - Offset))' if rect is not None else 'OFF'))
    imu = load_imu(a.imu, a.imu_offset_ns, rect)
    with open(os.path.join(a.out, 'mav0/imu0/data.csv'), 'w') as f:
        f.write('#timestamp [ns],w_x,w_y,w_z,a_x,a_y,a_z\n')
        for t, gx, gy, gz, ax, ay, az in imu:
            f.write(f'{t},{gx:.9f},{gy:.9f},{gz:.9f},{ax:.9f},{ay:.9f},{az:.9f}\n')
    span = (pairs[-1][0]['ts'] - t0) / 1e9
    print(f"wrote {len(pairs)} stereo pairs over {span:.1f}s ({len(pairs)/span:.1f} Hz), "
          f"{len(imu)} IMU samples -> {a.out}")


if __name__ == '__main__':
    main()
