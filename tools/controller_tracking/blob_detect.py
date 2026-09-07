#!/usr/bin/env python3
"""blob_detect.py — find IR-LED blobs in a controller-tracking exposure frame.

The ~38 us exposure class (research-notes/55, research-notes/41's short-exposure class taken to
its extreme) leaves the background essentially black (mean ~4/255) with the controller's IR LEDs as
a handful of small, near-saturated points -- visually confirmed on real captures
(exports/controller-constellation-2026-09-07/cap9). No ambient-light thresholding tricks are needed;
a fixed intensity threshold plus connected components is enough.

No blob detector existed anywhere in this repo before this file (confirmed by research before
writing it).
"""
import numpy as np
import cv2

# Background in the 38us class measures mean ~4.2-4.4, p99 ~8-9 on real captures; blobs saturate
# at 255. A wide margin between those two numbers, so this isn't a tuned/fragile threshold.
DEFAULT_THRESH = 40
MIN_AREA = 1
MAX_AREA = 200   # a real LED blob is a few pixels; anything bigger is glare/reflection, not a point


def find_blobs(img, thresh=DEFAULT_THRESH, min_area=MIN_AREA, max_area=MAX_AREA):
    """img: HxW uint8 grayscale (already cropped of any metadata row).

    Returns an (N,4) array of [u, v, area, peak_intensity], u/v intensity-weighted centroids
    (sub-pixel), one row per detected blob.
    """
    _, mask = cv2.threshold(img, thresh, 255, cv2.THRESH_BINARY)
    n, labels, stats, _ = cv2.connectedComponentsWithStats(mask, connectivity=8)
    out = []
    for i in range(1, n):  # label 0 is background
        area = stats[i, cv2.CC_STAT_AREA]
        if not (min_area <= area <= max_area):
            continue
        x0, y0, w, h = stats[i, cv2.CC_STAT_LEFT], stats[i, cv2.CC_STAT_TOP], \
            stats[i, cv2.CC_STAT_WIDTH], stats[i, cv2.CC_STAT_HEIGHT]
        patch = img[y0:y0+h, x0:x0+w].astype(np.float64)
        lbl_patch = labels[y0:y0+h, x0:x0+w]
        wgt = np.where(lbl_patch == i, patch, 0.0)
        total = wgt.sum()
        if total <= 0:
            continue
        yy, xx = np.mgrid[y0:y0+h, x0:x0+w]
        u = (wgt * xx).sum() / total
        v = (wgt * yy).sum() / total
        out.append([u, v, float(area), float(patch.max())])
    return np.array(out) if out else np.zeros((0, 4))


if __name__ == '__main__':
    import sys
    im = cv2.imread(sys.argv[1], cv2.IMREAD_GRAYSCALE)
    if im is None:
        sys.exit(f"cannot read {sys.argv[1]}")
    blobs = find_blobs(im)
    print(f"{len(blobs)} blobs")
    for u, v, area, peak in blobs:
        print(f"  ({u:7.2f}, {v:7.2f})  area={area:.0f}  peak={peak:.0f}")
