import sys
import unittest
from pathlib import Path
import numpy as np
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'tools/controller_tracking'))
from temporal_hypotheses import select_path


def hypothesis(angle=0,error=.001):
    c,s=np.cos(angle),np.sin(angle)
    return dict(rotation=[[c,-s,0],[s,c,0],[0,0,1]],position=[0,0,.7],error=error,score=[-6,-12,error])


class TemporalTests(unittest.TestCase):
    def test_supported_flip_is_not_forced_into_path(self):
        frames=[dict(ts_ns=1_000_000_000+i*40_000_000,hypotheses=[hypothesis()]) for i in range(5)]
        frames[2]['hypotheses']=[hypothesis(np.pi)]
        path=select_path(frames)
        self.assertIsNone(path[2])
        self.assertTrue(all(path[i] is not None for i in (0,1,3,4)))

    def test_missing_frame_is_not_interpolated(self):
        frames=[dict(ts_ns=1_000_000_000+i*40_000_000,hypotheses=[hypothesis()]) for i in range(3)]
        frames[1]['hypotheses']=[]
        self.assertIsNone(select_path(frames)[1])

    def test_alternate_smooth_pose_survives_lower_error_flip(self):
        frames=[dict(ts_ns=1_000_000_000+i*40_000_000,hypotheses=[hypothesis(.02*i)]) for i in range(5)]
        frames[2]['hypotheses'].insert(0,hypothesis(np.pi,.0001))
        path=select_path(frames)
        np.testing.assert_allclose(path[2]['R'],np.array(hypothesis(.04)['rotation']))

class FixedLagTests(unittest.TestCase):
    def test_latency_and_end_of_stream_are_explicit(self):
        from temporal_hypotheses import FixedLagTracker
        tracker=FixedLagTracker()
        frames=[dict(ts_ns=1_000_000_000+i*40_000_000,hypotheses=[hypothesis(.01*i)]) for i in range(5)]
        self.assertIsNone(tracker.push(frames[0]));self.assertIsNone(tracker.push(frames[1]))
        row=tracker.push(frames[2]);self.assertEqual(row[0]['ts_ns'],frames[0]['ts_ns'])
        self.assertIsNotNone(row[1])
        tracker.push(frames[3]);tracker.push(frames[4])
        self.assertTrue(all(pose is None for frame,pose in tracker.finish()))
