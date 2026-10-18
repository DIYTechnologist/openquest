import sys
import unittest
from pathlib import Path
import cv2
import numpy as np
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'tools/controller_tracking'))
from gyro_hypotheses import GyroIntegrator,select_gyro


class GyroTests(unittest.TestCase):
    def test_constant_body_rotation_integrates_without_scale_fit(self):
        t=np.linspace(1,2,501);omega=np.tile([0,0,np.pi/2],(len(t),1))
        gyro=GyroIntegrator(t,omega)
        np.testing.assert_allclose(gyro.delta(1,2),cv2.Rodrigues(np.array([0,0,np.pi/2]))[0],atol=1e-12)

    def test_gaps_and_nonmonotonic_input_are_rejected(self):
        gyro=GyroIntegrator([1,1.02],[[0,0,0],[0,0,0]])
        self.assertIsNone(gyro.delta(1,1.02))
        self.assertIsNone(gyro.delta(.9,1))
        with self.assertRaises(ValueError):GyroIntegrator([1,1],[[0,0,0],[0,0,0]])

    def test_camera_loss_is_not_filled_with_invented_position(self):
        gyro=GyroIntegrator(np.linspace(1,1.1,51),np.zeros((51,3)))
        h=dict(rotation=np.eye(3).tolist(),position=[0,0,.7],error=.001,score=[-8,-16,.001])
        frames=[dict(ts_ns=1_000_000_000,hypotheses=[h]),dict(ts_ns=1_040_000_000,hypotheses=[])]
        path=select_gyro(frames,gyro)
        self.assertIsNotNone(path[0]);self.assertIsNone(path[1])

    def test_optically_plausible_flip_is_rejected_by_gyro(self):
        gyro=GyroIntegrator(np.linspace(1,1.1,51),np.zeros((51,3)))
        a=dict(rotation=np.eye(3).tolist(),position=[0,0,.7],error=.001,score=[-8,-16,.001])
        b=dict(a,rotation=cv2.Rodrigues(np.array([0,0,np.pi]))[0].tolist())
        frames=[dict(ts_ns=1_000_000_000,hypotheses=[a]),dict(ts_ns=1_040_000_000,hypotheses=[b])]
        self.assertIsNone(select_gyro(frames,gyro)[1])

class RelativePredictionTests(unittest.TestCase):
    def test_head_rotation_changes_relative_pose_with_correct_sign(self):
        from gyro_hypotheses import relative_prediction
        delta=cv2.Rodrigues(np.array([0,.2,0]))[0]
        np.testing.assert_allclose(relative_prediction(np.eye(3),np.eye(3),delta),delta.T)

    def test_noncommuting_head_and_controller_frames(self):
        from gyro_hypotheses import relative_prediction
        Rh=cv2.Rodrigues(np.array([.1,.2,-.3]))[0];Rc=cv2.Rodrigues(np.array([-.2,.3,.1]))[0]
        dh=cv2.Rodrigues(np.array([.02,-.01,.03]))[0];dc=cv2.Rodrigues(np.array([-.04,.01,.02]))[0]
        np.testing.assert_allclose(relative_prediction(Rh.T@Rc,dc,dh),(Rh@dh).T@(Rc@dc),atol=1e-12)
