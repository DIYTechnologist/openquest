#!/usr/bin/env python3
"""build_euroc_direct.py — turn a cam_direct capture into a Basalt/EuRoC dataset.

Input:  <capture>/frames.csv, <capture>/raw/*.gray, <capture>/syncboss.raw
Output: <out>/mav0/{cam0,cam1}/data/<ts_ns>.png + data.csv, <out>/mav0/imu0/data.csv

Two capture-specific facts drive this (see notes/11):
  * Frames alternate long-exposure SLAM and short-exposure controller-IR frames at ~60 Hz.
    Only the bright half is usable for VIO -> filter on mean intensity.
  * Frames are stamped CLOCK_MONOTONIC; the IMU (syncboss type 0x50) is stamped on the nRF
    1 MHz clock. Syncboss type 0xe0 carries the per-strobe exposure time on the nRF clock at
    the SLAM frame rate, so pairing 0xe0 against the bright frames gives the affine map
    mono_ns = a*nrf_us + b, which we then apply to the IMU.
"""
import csv, os, struct, sys, zlib, math

W, H_FULL, H = 640, 481, 480
# The metadata row is row 0, NOT row 480: it reads mean~6 with a distinctive 00 00 01 ff ff ff 01 a5
# marker, while rows 1..480 carry image data. Dropping the last row instead left the garbage row in
# as row 0 and shifted every image one row off the calibration.
META_ROWS = 1                          # skip this many leading rows
BRIGHT_MIN = 20.0                     # mean intensity separating SLAM from controller frames
IMU_LEAD_NS = 500_000_000             # require this much IMU history before the first frame
# The nRF clock starts near zero, so raw timestamps span 1e9..2.5e10 ns and change digit COUNT
# mid-dataset (1000178000 -> 10000088000). Frame files are named by timestamp, so any consumer
# that sorts lexicographically would order the sequence wrongly. Shift everything by a constant
# so all timestamps are the same width (the known-good EuRoC datasets are uniformly 14-15 digits).
# Relative timing is untouched.
TIME_BASE_NS = 100_000_000_000_000
# Camera->IMU time offset, determined empirically by cross-correlating optical-flow magnitude
# against gyro magnitude (tools/vio/cam_imu_lag.py): correlation rises from 0.38 at zero lag to
# 0.95 at +816 ms. So the 0xe0 exposure stamps and the 0x50 IMU stamps do NOT share an epoch,
# despite both being u32 microsecond fields in the same syncboss stream. This offset is ~24.5
# frame periods, which is why the earlier +/-2 frame sweeps could never find it.
CAM_SHIFT_NS = int(os.environ.get('CAM_SHIFT_MS', '816')) * 1_000_000
G, DEG = 9.80665, math.pi / 180.0


FLIP = os.environ.get('FLIP', '')      # 'v', 'h', or 'vh' — the v4l2 path may deliver rows in a
                                       # different order than the ImageBuffer path the calibration
                                       # was derived from; a flip would break stereo geometry.

def write_png(gray, path):
    rows = [gray[(y + META_ROWS) * W:(y + META_ROWS + 1) * W] for y in range(H)]
    if 'v' in FLIP:
        rows = rows[::-1]
    if 'h' in FLIP:
        rows = [r[::-1] for r in rows]
    raw = b''.join(b'\x00' + r for r in rows)
    def ck(t, d):
        return struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d) & 0xffffffff)
    with open(path, 'wb') as f:
        f.write(b'\x89PNG\r\n\x1a\n'
                + ck(b'IHDR', struct.pack('>IIBBBBB', W, H, 8, 0, 0, 0, 0))
                + ck(b'IDAT', zlib.compress(raw, 1)) + ck(b'IEND', b''))


def parse_syncboss(path):
    """-> (list of nRF exposure ts in us, list of (ts_us, gyro3, accel3))"""
    d = open(path, 'rb').read()
    exp, imu = [], []
    i, n = 0, len(d)
    while i + 6 <= n:
        if not (d[i] == 1 and d[i+1] == 3 and d[i+2] == 0 and d[i+4] == 0):
            i += 1
            continue
        t, L = d[i+3], d[i+5]
        if i + 6 + L > n:
            break
        pl = d[i+6:i+6+L]
        i += 6 + L
        if t == 0xe0 and L == 14:
            # {u8 flags; u32 ts_us; u32 pad; u32 frame_counter; u8}
            ts, = struct.unpack('<I', pl[1:5])
            cnt, = struct.unpack('<I', pl[9:13])
            exp.append((ts, cnt))
        elif t == 0x50 and L == 36:
            ts, = struct.unpack('<I', pl[:4])
            ax, ay, az, gx, gy, gz, _temp = struct.unpack('<7f', pl[8:36])
            imu.append((ts, (gx*DEG, gy*DEG, gz*DEG), (ax*G, ay*G, az*G)))
    return exp, imu


def unwrap_us(seq):
    """nRF ts is u32 microseconds; undo 32-bit wraps."""
    out, wrap, last = [], 0, None
    for t in seq:
        if last is not None and t < last - (1 << 28):
            wrap += 1
        last = t
        out.append(t + (wrap << 32))
    return out


def fit_clock(frame_mono_ns, exp_us):
    """Least-squares mono_ns = a*nrf_us + b, trying small index alignments."""
    best = None
    for shift in range(-8, 9):
        xs, ys = [], []
        for k, m in enumerate(frame_mono_ns):
            j = k + shift
            if 0 <= j < len(exp_us):
                xs.append(exp_us[j]); ys.append(m)
        if len(xs) < 50:
            continue
        n = len(xs)
        mx, my = sum(xs)/n, sum(ys)/n
        sxx = sum((x-mx)**2 for x in xs)
        sxy = sum((x-mx)*(y-my) for x, y in zip(xs, ys))
        if sxx == 0:
            continue
        a = sxy/sxx; b = my - a*mx
        res = [abs(a*x + b - y) for x, y in zip(xs, ys)]
        res.sort()
        med = res[len(res)//2]
        if best is None or med < best[0]:
            best = (med, a, b, shift, n)
    return best


def main(cap, out):
    rows = [r for r in csv.reader(open(os.path.join(cap, 'frames.csv')))
            if r and not r[0].startswith('#')]
    raw = os.path.join(cap, 'raw')

    # classify frames and keep the bright (SLAM) ones
    per_cam = {}
    for seq, camid, ts, fn in rows:
        p = os.path.join(raw, fn)
        if not os.path.exists(p):
            continue
        d = open(p, 'rb').read()
        if len(d) != W * H_FULL:
            continue
        m = sum(d[W*META_ROWS:W*(META_ROWS+H):64]) / (W*H/64)
        if m < BRIGHT_MIN:
            continue
        per_cam.setdefault(camid, []).append((int(ts), p))
    for c in per_cam:
        per_cam[c].sort()
        print(f"cam{c}: {len(per_cam[c])} SLAM frames")

    cams = sorted(per_cam)
    if len(cams) < 2:
        print("need two cameras"); return 1

    exp, imu = parse_syncboss(os.path.join(cap, 'syncboss.raw'))
    exp_us = unwrap_us([e[0] for e in exp])
    imu_us = unwrap_us([s[0] for s in imu])
    print(f"syncboss: {len(exp_us)} exposure packets, {len(imu_us)} IMU samples")

    # --- timebase -------------------------------------------------------------------------
    # Frame stamps from the V4L2 buffer are COMPLETION times and jitter by ~ms against the true
    # exposure instant; notes/08 records that exact jitter as the sole VIO convergence blocker.
    # The MCU gives us the real thing: 0xe0 carries the per-strobe exposure time on the nRF clock.
    # So snap every frame to its 0xe0 exposure and express the whole dataset in the nRF timebase
    # (ts_ns = nrf_us * 1000) — the same convention as the known-good exports/vio-precise dataset,
    # which removes the mono<->nRF mapping (and its integer-frame-shift ambiguity) entirely.
    ref = [t for t, _ in per_cam[cams[0]]]
    fit = fit_clock(ref, exp_us)
    if not fit:
        print("clock fit failed"); return 1
    med, a, b, shift, n = fit
    print(f"clock map: mono_ns = {a:.6f}*nrf_us + {b:.1f}  "
          f"(median residual {med/1e6:.3f} ms, {n} pairs, shift {shift})")

    # EuRoC stereo requires the two cameras to share IDENTICAL timestamps: Basalt pairs frames by
    # exact timestamp, so a few-hundred-microsecond difference makes it drop nearly every pair.
    # (First attempt: only 5 of 753 matched exactly -> Basalt emitted exactly 5 poses.)
    # The exposures really are simultaneous — the FSIN strobe fires both cameras together — and the
    # residual is buffer-completion jitter in our own dequeue-side timestamping. So pair by nearest
    # neighbour within a tolerance and stamp both with the cam0 time. Unpaired frames are dropped.
    import bisect
    PAIR_TOL_NS = 4_000_000            # 4 ms: well under the 33 ms frame period, well over the
                                       # observed 118 us median offset
    a_list, b_list = per_cam[cams[0]], per_cam[cams[1]]
    b_ts = [t for t, _ in b_list]
    paired = []
    for ts, pa in a_list:
        j = bisect.bisect_left(b_ts, ts)
        best, bd = None, None
        for k in (j - 1, j):
            if 0 <= k < len(b_ts):
                dd = abs(b_ts[k] - ts)
                if bd is None or dd < bd:
                    bd, best = dd, k
        if best is not None and bd <= PAIR_TOL_NS:
            paired.append((ts, pa, b_list[best][1]))
    print(f"stereo pairs within {PAIR_TOL_NS/1e6:.0f} ms: {len(paired)} "
          f"(from {len(a_list)}/{len(b_list)})")
    # Keep the PAIRED structure from here on. Snapping, dedup and the IMU-lead trim used to be
    # applied to each camera independently, which happened to leave equal-length lists for this
    # capture but could silently diverge on another one — and Basalt pairs stereo frames by exact
    # timestamp, so divergence would be near-impossible to spot. One list, one timestamp per pair.
    KOFF = int(os.environ.get('KOFF', '0'))
    exp_ns = [t * 1000 + TIME_BASE_NS + CAM_SHIFT_NS for t in exp_us]

    def snap(ts_mono):
        nrf_est = (ts_mono - b) / a
        j = bisect.bisect_left(exp_us, nrf_est)
        best, bd = None, None
        for k in (j - 1, j, j + 1):
            if 0 <= k < len(exp_us):
                dd_ = abs(exp_us[k] - nrf_est)
                if bd is None or dd_ < bd:
                    bd, best = dd_, k
        if best is None:
            return None
        k = best + KOFF
        return exp_ns[k] if 0 <= k < len(exp_ns) else None

    # Basalt needs IMU history covering the first frame; the FSIN strobe starts before the IMU
    # stream comes up, so frames without preceding IMU are unusable and get dropped.
    cut = imu_us[0] * 1000 + TIME_BASE_NS + IMU_LEAD_NS
    snapped, seen, dropped_lead, dropped_dup = [], set(), 0, 0
    for ts, pa, pb in paired:
        e = snap(ts)                       # one snap per PAIR, so both cameras get one timestamp
        if e is None:
            continue
        if e < cut:
            dropped_lead += 1
            continue
        if e in seen:                      # a stale buffer can land two frames on one strobe
            dropped_dup += 1
            continue
        seen.add(e)
        snapped.append((e, pa, pb))
    snapped.sort()
    print(f"snapped to 0xe0 exposure times (KOFF={KOFF}, cam shift {CAM_SHIFT_NS/1e6:.0f} ms); "
          f"dropped {dropped_lead} lacking {IMU_LEAD_NS/1e9:.2f}s IMU lead, {dropped_dup} duplicate; "
          f"{len(snapped)} stereo pairs")
    per_cam[cams[0]] = [(t, pa) for t, pa, _ in snapped]
    per_cam[cams[1]] = [(t, pb) for t, _, pb in snapped]

    # Invariant Basalt depends on: identical, strictly increasing timestamps in both cameras.
    ts0 = [t for t, _ in per_cam[cams[0]]]
    ts1 = [t for t, _ in per_cam[cams[1]]]
    assert ts0 == ts1, "cam0/cam1 timestamp lists diverged"
    assert all(y > x for x, y in zip(ts0, ts0[1:])), "timestamps not strictly increasing"

    for idx, c in enumerate(cams[:2]):
        dd = os.path.join(out, 'mav0', f'cam{idx}', 'data')
        os.makedirs(dd, exist_ok=True)
        with open(os.path.join(out, 'mav0', f'cam{idx}', 'data.csv'), 'w') as f:
            f.write('#timestamp [ns],filename\n')
            for ts, p in per_cam[c]:
                write_png(open(p, 'rb').read(), os.path.join(dd, f'{ts}.png'))
                f.write(f'{ts},{ts}.png\n')
        print(f"wrote cam{idx} ({len(per_cam[c])} png) from source cam{c}")

    os.makedirs(os.path.join(out, 'mav0', 'imu0'), exist_ok=True)
    with open(os.path.join(out, 'mav0', 'imu0', 'data.csv'), 'w') as f:
        f.write('#timestamp [ns],w_x,w_y,w_z,a_x,a_y,a_z\n')
        for us, g, acc in zip(imu_us, [s[1] for s in imu], [s[2] for s in imu]):
            f.write('%d,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g\n'
                    % (us * 1000 + TIME_BASE_NS, g[0], g[1], g[2], acc[0], acc[1], acc[2]))
    print(f"wrote imu0 ({len(imu_us)} samples)")
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1], sys.argv[2]))
