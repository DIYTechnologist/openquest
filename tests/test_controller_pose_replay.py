"""Synthetic replay/reference tests; no hardware performance claim."""
import io
import json
import sys
import tempfile
import unittest
from pathlib import Path
import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools/controller_tracking'))
from pose_geometry import rotate_reference_offset
from replay_bearings import replay
from controller_ate import load_pnp_poses


class ReplayTests(unittest.TestCase):
    def test_reference_offset_rotates_and_quaternion_sign_is_irrelevant(self):
        q = [0, 0, np.sin(np.pi / 4), np.cos(np.pi / 4)]
        for sign in (1, -1):
            np.testing.assert_allclose(rotate_reference_offset(np.array(q) * sign, [1, 0, 0]),
                                       [0, 1, 0], atol=1e-14)
        with self.assertRaises(ValueError):
            rotate_reference_offset([0, 0, 0, 0], [1, 0, 0])

    def test_ate_uses_rotated_calibrated_offset(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'poses.csv'
            path.write_text('10,4,4,0,brute,1,2,3,0,0,0,1\n')
            result = load_pnp_poses(path, {10: 20}, [0.1, 0.2, 0.3])
            np.testing.assert_allclose(result, [[20, 1.1, 2.2, 3.3]])

    def stream(self, reject=False):
        square = np.array([[-.04, -.04, 0], [.04, -.04, 0], [.04, .04, 0], [-.04, .04, 0]])
        points = square + [0.03, .01, .7]
        dirs = points / np.linalg.norm(points, axis=1, keepdims=True)
        rows = [dict(type='config', schema=1, reference='centroid', model_points=square.tolist(),
                     camera_to_imu=np.eye(4).tolist(), tracker_parameters={'reject_ambiguous': reject}),
                dict(type='frame', ts_ns=1, bearings=dirs.tolist(), searched_bearings=dirs.tolist()),
                dict(type='frame', ts_ns=2, bearings=[], searched_bearings=[])]
        return io.StringIO('\n'.join(map(json.dumps, rows)))

    def test_replay_reports_ambiguity_and_empty_frame(self):
        output = io.StringIO()
        stats = replay(self.stream(), output)
        self.assertEqual(stats['frames'], 2)
        self.assertEqual(stats['solved'], 1)
        self.assertEqual(stats['ambiguous'], 1)
        self.assertIsNone(json.loads(output.getvalue().splitlines()[1])['pose'])

    def test_replay_honors_recorded_rejection_and_override(self):
        self.assertEqual(replay(self.stream(True), io.StringIO())['solved'], 0)
        self.assertEqual(replay(self.stream(True), io.StringIO(), False)['solved'], 1)


if __name__ == '__main__':
    unittest.main()
