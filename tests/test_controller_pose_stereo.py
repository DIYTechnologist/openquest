"""Synthetic second-camera support tests; no physical accuracy claim."""
import sys
import unittest
from pathlib import Path
import numpy as np
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools/controller_tracking'))
from stereo_audit import stereo_support


class StereoTests(unittest.TestCase):
    def test_camera_transform_and_distinct_matches(self):
        transform = np.eye(4); transform[:3, 3] = [.1, 0, 0]
        points = np.array([[0, 0, 1], [.1, .1, 1], [-.1, -.1, 1], [.2, 0, 1]])
        rays = points - transform[:3, 3]
        result = stereo_support(points, transform, rays)
        self.assertEqual(result['matched'], 4)
        self.assertLess(result['mean_normalized_error'], 1e-12)
        self.assertEqual(stereo_support(points + [1, 0, 0], transform, rays)['matched'], 0)

    def test_one_observation_cannot_support_multiple_leds(self):
        points = np.array([[0, 0, 1], [.001, 0, 1], [.002, 0, 1]])
        self.assertEqual(stereo_support(points, np.eye(4), np.array([[0, 0, 1]]))['matched'], 1)

    def test_empty_and_behind_camera(self):
        for points, observed in ((np.array([[0, 0, -1]]), np.array([[0, 0, 1]])),
                                 (np.array([[0, 0, 1]]), np.zeros((0, 3)))):
            self.assertEqual(stereo_support(points, np.eye(4), observed)['matched'], 0)

class StereoVetoTests(unittest.TestCase):
    def test_pose_is_checked_in_other_camera_coordinates(self):
        from stereo_track import stereo_check
        model = np.array([[-.04,-.03,0],[.04,-.03,0],[.04,.03,0],[-.04,.03,0]])
        a = np.eye(4); a[:3,3] = [.02,0,0]
        b = np.eye(4); b[:3,3] = [.12,0,0]
        t = np.array([0,0,.7])
        observed = model+t+a[:3,3]-b[:3,3]
        check = stereo_check(model,a,b,observed)
        self.assertTrue(check(np.eye(3),t))
        self.assertFalse(check(np.eye(3),t+[.2,0,0]))

    def test_bruteforce_does_not_return_rejected_candidate(self):
        from pnp_track import track_frame
        model = np.array([[-.04,-.03,0],[.04,-.03,0],[.04,.03,0],[-.04,.03,0]])
        observed = model + [0,0,.7]
        result = track_frame(model,observed,4,.01,candidate_check=lambda R,t:False)
        self.assertIsNone(result)
