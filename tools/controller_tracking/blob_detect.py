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


def find_static_positions(frame_reader, frame_rows, sample_n=15, tol=3.0, min_frac=0.7,
                           detect_kwargs=None):
    """Positions that recur, essentially unmoved, across most of a spread of sampled frames --
    the signature of a fixed IR source in the scene (research-notes/58 found this the hard way: a
    5-blob cluster sat at the same pixel position for an entire ~500-frame, 20s capture, almost
    certainly the OTHER controller sitting idle in view rather than the one actually being tracked
    and moved -- both controllers can be lit and visible at once). A blob detector alone cannot
    tell "real LED" from "someone else's real LED that isn't the one we're tracking"; only motion
    can, since the camera itself is fixed for the whole capture (desk-mounted, proximity-bypassed).

    frame_reader(row) -> HxW grayscale image or None.
    Returns an (N,2) array of static (u, v) positions to exclude via filter_static.
    """
    detect_kwargs = detect_kwargs or {}
    idxs = np.linspace(0, len(frame_rows) - 1, min(sample_n, len(frame_rows))).astype(int)
    all_pts = []
    for i in idxs:
        im = frame_reader(frame_rows[i])
        if im is None:
            continue
        b = find_blobs(im, **detect_kwargs)
        if len(b):
            all_pts.append(b[:, :2])
    if len(all_pts) < 3:
        return np.zeros((0, 2))

    pts = np.concatenate(all_pts, axis=0)
    static = []
    used = np.zeros(len(pts), dtype=bool)
    for i in range(len(pts)):
        if used[i]:
            continue
        d = np.linalg.norm(pts - pts[i], axis=1)
        group = d < tol
        # A real static source is seen in most sampled frames, not just clustered within one --
        # count DISTINCT source frames represented, not raw point count (one frame can contribute
        # more than one nearby point if a blob is detected as two adjacent components).
        frac = group.sum() / len(all_pts)
        if frac >= min_frac:
            static.append(pts[group].mean(axis=0))
            used |= group
    return np.array(static) if static else np.zeros((0, 2))


def filter_static(blobs, static_positions, tol=5.0):
    """Drop detected blobs within tol pixels of any known-static position."""
    if len(static_positions) == 0 or len(blobs) == 0:
        return blobs
    d = np.linalg.norm(blobs[:, None, :2] - static_positions[None, :, :], axis=2)
    keep = d.min(axis=1) > tol
    return blobs[keep]


if __name__ == '__main__':
    import sys
    im = cv2.imread(sys.argv[1], cv2.IMREAD_GRAYSCALE)
    if im is None:
        sys.exit(f"cannot read {sys.argv[1]}")
    blobs = find_blobs(im)
    print(f"{len(blobs)} blobs")
    for u, v, area, peak in blobs:
        print(f"  ({u:7.2f}, {v:7.2f})  area={area:.0f}  peak={peak:.0f}")
