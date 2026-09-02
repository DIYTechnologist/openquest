#!/usr/bin/env python3
"""make_pair_calib.py — build a Basalt 2-camera calib for an arbitrary ordered camera pair.

quest_calib_convert.py emits all four cameras but omits the IMU noise/rate fields Basalt
requires (it fails with: JSON Parsing failed - provided NVP (imu_update_rate) not found).
The ad-hoc calib_XY.json files in exports/vio-stereo-2026-09-01/ do carry those fields but only
cover 5 of the 6 unordered pairs ({1,2} is missing) in one order. So: take a complete pair file
as a template and inject the desired cameras from the 4-camera calibration.

Usage: make_pair_calib.py <template.json> <basalt_calibration.json> <camA> <camB> <out.json>
"""
import json, sys


def main(tmpl, src, a, b, out):
    t = json.load(open(tmpl))
    s = json.load(open(src))['value0']
    v = t['value0']
    for key in ('T_imu_cam', 'intrinsics', 'resolution', 'vignette'):
        if key in s and isinstance(s[key], list) and len(s[key]) >= max(a, b) + 1:
            v[key] = [s[key][a], s[key][b]]
    json.dump(t, open(out, 'w'), indent=1)


if __name__ == '__main__':
    main(sys.argv[1], sys.argv[2], int(sys.argv[3]), int(sys.argv[4]), sys.argv[5])
