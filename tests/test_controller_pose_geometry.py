"""Synthetic geometry tests. These do not validate tracking on real images."""
import sys
from pathlib import Path
import unittest

import cv2
import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools/controller_tracking'))
from pose_geometry import model_reference, reprojection_error, HypothesisBank
from pnp_track import solve_pose, solve_pose_candidates, track_frame, track_frame_prior


POINTS = np.array([[-.04, -.025, .006], [.038, -.03, -.004], [.049, .018, .012],
                   [.005, .045, -.008], [-.043, .027, .015], [-.012, -.008, -.02]])
POINTS -= POINTS.mean(axis=0)


def rotation(vector):
    return cv2.Rodrigues(np.array(vector, dtype=float))[0]


def bearings(points, R, t):
    camera = points @ R.T + t
    return camera / np.linalg.norm(camera, axis=1, keepdims=True)


class PoseGeometryTests(unittest.TestCase):
    def test_centering_preserves_camera_points_exactly(self):
        raw = POINTS + [.3, -.2, .6]
        centered, offset = model_reference(raw)
        R, t = rotation([.2, -.3, .1]), np.array([.04, -.01, .7])
        np.testing.assert_allclose(raw @ R.T + t,
                                   centered @ R.T + (t + R @ offset), atol=1e-14)
        np.testing.assert_allclose(centered.mean(axis=0), 0, atol=1e-14)

    def test_rotation_about_stationary_centroid_has_no_artificial_translation(self):
        raw = POINTS + [.3, -.1, .6]
        centered, offset = model_reference(raw)
        position = np.array([.1, .05, .8])
        corrected, historical = [], []
        for angle in np.linspace(-1.2, 1.2, 15):
            R = rotation([.1, angle, -.1])
            observed = bearings(centered, R, position)
            new = solve_pose(centered, observed)
            old = solve_pose(raw, observed)
            self.assertIsNotNone(new)
            self.assertIsNotNone(old)
            np.testing.assert_allclose(new[1], position, atol=1e-6)
            np.testing.assert_allclose(old[1], position - R @ offset, atol=1e-6)
            corrected.append(new[1]); historical.append(old[1])
        self.assertLess(np.ptp(np.array(corrected), axis=0).max(), 1e-6)
        self.assertGreater(np.ptp(np.array(historical), axis=0).max(), .3)

    def test_metric_translation_and_orientation_on_distinct_test_poses(self):
        # No alignment, scale fit, ground-truth feedback or tuned thresholds.
        rng = np.random.default_rng(20261017)
        for _ in range(40):
            R = rotation(rng.uniform(-.6, .6, 3))
            t = rng.uniform([-.1, -.1, .45], [.1, .1, 1.3])
            result = solve_pose(POINTS, bearings(POINTS, R, t))
            self.assertIsNotNone(result)
            np.testing.assert_allclose(result[0], R, atol=1e-6)
            np.testing.assert_allclose(result[1], t, atol=1e-6)

    def test_legacy_mode_preserves_original_model_coordinates(self):
        raw = POINTS + [1, 2, 3]
        model, origin = model_reference(raw, 'legacy')
        np.testing.assert_array_equal(model, raw)
        np.testing.assert_array_equal(origin, np.zeros(3))

    def test_behind_camera_pose_is_rejected_even_with_zero_projection_error(self):
        t = np.array([0, 0, -1.])
        p = POINTS @ np.eye(3) + t
        dirs = p / np.linalg.norm(p, axis=1, keepdims=True)
        self.assertIsNone(reprojection_error(POINTS, dirs, np.eye(3), t))

    def test_nonfinite_and_invalid_bearings_rejected(self):
        valid = bearings(POINTS, np.eye(3), np.array([0, 0, .6]))
        invalid = valid.copy(); invalid[0, 0] = np.nan
        self.assertEqual(solve_pose_candidates(POINTS, invalid), [])
        invalid = valid.copy(); invalid[0, 2] = 0
        self.assertEqual(solve_pose_candidates(POINTS, invalid), [])

    def test_bad_models_refused(self):
        for points in ([], [[0, 0, 0]], [[0, 0, float('nan')]] * 3):
            with self.assertRaises(ValueError):
                model_reference(points)

    def test_distinct_pose_ambiguity_is_not_hidden(self):
        bank = HypothesisBank()
        bank.add((np.eye(3), np.array([0., 0., 1.]), .001))
        bank.add((rotation([0, 0, .8]), np.array([.1, 0, 1.]), .00105))
        self.assertTrue(bank.report()['ambiguous'])
        self.assertEqual(bank.report()['distinct_shortlisted'], 2)

    def test_repeated_same_pose_does_not_create_false_ambiguity(self):
        bank = HypothesisBank()
        for error in (.003, .002, .001):
            bank.add((np.eye(3), np.array([0., 0., 1.]), error))
        self.assertEqual(bank.report()['accepted_candidates'], 3)
        self.assertEqual(bank.report()['distinct_shortlisted'], 1)
        self.assertFalse(bank.report()['ambiguous'])

    def test_real_pnp_square_correspondence_symmetry_is_reported(self):
        square = np.array([[-.04, -.04, 0], [.04, -.04, 0], [.04, .04, 0], [-.04, .04, 0]])
        observed = bearings(square, rotation([.15, -.1, .2]), np.array([.03, .01, .7]))
        diagnostic = {}
        result = track_frame(square, observed, 4, .001, max_k=4, diagnostics=diagnostic)
        self.assertIsNotNone(result)
        self.assertTrue(diagnostic['ambiguous'])
        self.assertGreater(diagnostic['rotation_separation_deg'], 15)

    def test_asymmetric_model_prior_tracks_known_small_motion(self):
        R, t = rotation([.1, .05, .01]), np.array([.02, -.01, .7])
        result = track_frame_prior(POINTS, bearings(POINTS, R, t), np.eye(3),
                                   np.array([.02, -.01, .7]), .03, 4, .001)
        self.assertIsNotNone(result)
        np.testing.assert_allclose(result[1], t, atol=1e-6)


if __name__ == '__main__':
    unittest.main()
