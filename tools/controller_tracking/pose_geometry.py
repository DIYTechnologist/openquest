"""Explicit pose reference points and distinct-hypothesis diagnostics."""
import numpy as np


def model_reference(points, reference='centroid'):
    points = np.asarray(points, dtype=np.float64)
    if points.ndim != 2 or points.shape[1] != 3 or len(points) < 3 or not np.isfinite(points).all():
        raise ValueError('Model must contain at least three finite 3D points')
    if reference not in ('centroid', 'legacy'):
        raise ValueError('Unknown model reference')
    origin = points.mean(axis=0) if reference == 'centroid' else np.zeros(3)
    return points - origin, origin


def reprojection_error(points, bearings, rotation, translation):
    """Mean normalized-image distance, rejecting non-finite/behind-camera poses."""
    points = np.asarray(points, dtype=np.float64)
    bearings = np.asarray(bearings, dtype=np.float64)
    rotation = np.asarray(rotation, dtype=np.float64)
    translation = np.asarray(translation, dtype=np.float64).reshape(3)
    if bearings.shape != points.shape or not all(np.isfinite(x).all() for x in
                                                  (points, bearings, rotation, translation)):
        return None
    if rotation.shape != (3, 3) or np.any(bearings[:, 2] <= 1e-9):
        return None
    camera_points = points @ rotation.T + translation
    if np.any(camera_points[:, 2] <= 1e-9):
        return None
    predicted = camera_points[:, :2] / camera_points[:, 2:3]
    observed = bearings[:, :2] / bearings[:, 2:3]
    return float(np.linalg.norm(predicted - observed, axis=1).mean())


def rotation_distance(a, b):
    cosine = np.clip((np.trace(a @ b.T) - 1) * .5, -1, 1)
    return float(np.arccos(cosine))


def rotate_reference_offset(quaternion_xyzw, offset):
    """Rotate a calibrated model-local reference offset into the reported pose frame."""
    q = np.asarray(quaternion_xyzw, dtype=float)
    offset = np.asarray(offset, dtype=float)
    if q.shape != (4,) or offset.shape != (3,) or not np.isfinite(q).all() or not np.isfinite(offset).all():
        raise ValueError('Invalid quaternion or reference offset')
    norm = np.linalg.norm(q)
    if norm < 1e-12:
        raise ValueError('Zero quaternion')
    q = q / norm
    cross = np.cross(q[:3], offset)
    return offset + 2 * (q[3] * cross + np.cross(q[:3], cross))


class HypothesisBank:
    """Bounded diagnostic shortlist, not a temporal filter or posterior."""
    def __init__(self, position_separation=.02, angle_separation=np.deg2rad(15), capacity=8):
        self.position_separation = position_separation
        self.angle_separation = angle_separation
        self.capacity = capacity
        self.poses = []
        self.evaluated = 0

    def add(self, pose):
        self.evaluated += 1
        R, t, error = pose
        for index, (old_R, old_t, old_error) in enumerate(self.poses):
            if (np.linalg.norm(t - old_t) < self.position_separation and
                    rotation_distance(R, old_R) < self.angle_separation):
                if error < old_error:
                    self.poses[index] = (R.copy(), t.copy(), float(error))
                break
        else:
            self.poses.append((R.copy(), t.copy(), float(error)))
        self.poses.sort(key=lambda pose: pose[2])
        del self.poses[self.capacity:]

    def report(self, error_margin=1e-4):
        report = {'accepted_candidates': self.evaluated, 'distinct_shortlisted': len(self.poses),
                  'ambiguous': False, 'error_margin': error_margin,
                  'position_separation_m': self.position_separation,
                  'angle_separation_deg': float(np.rad2deg(self.angle_separation))}
        if not self.poses:
            return report
        R, t, error = self.poses[0]
        report['best_error'] = error
        if len(self.poses) > 1:
            other_R, other_t, other_error = self.poses[1]
            report.update(alternative_error=other_error,
                          translation_separation_m=float(np.linalg.norm(t - other_t)),
                          rotation_separation_deg=float(np.rad2deg(rotation_distance(R, other_R))),
                          ambiguous=bool(other_error <= error + error_margin))
        return report
