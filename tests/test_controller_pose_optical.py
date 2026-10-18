import sys
import unittest
from pathlib import Path
import numpy as np
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'tools/controller_tracking'))
from gyro_optical import refine_optical

MODEL=np.array([[-.04,-.03,0],[.04,-.03,.005],[.04,.03,0],[-.04,.03,.005],[0,.05,-.01],[.05,0,.01]])


def view(points,T):
    rays=(points-T[:3,3])@T[:3,:3]
    return rays/np.linalg.norm(rays,axis=1,keepdims=True)


class OpticalTests(unittest.TestCase):
    def test_separate_led_sets_can_constrain_same_pose(self):
        TA=np.eye(4);TB=np.eye(4);TB[0,3]=.1;t=np.array([.02,-.01,.7]);points=MODEL+t
        views=[(TA,view(points[:3],TA)),(TB,view(points[3:],TB))]
        result=refine_optical(MODEL,None,np.eye(3),t+[.002,-.001,.003],views)
        self.assertIsNotNone(result)
        np.testing.assert_allclose(result[1],t,atol=1e-6)
        np.testing.assert_allclose(result[0],np.eye(3),atol=1e-6)

    def test_monocular_known_orientation_can_recover_metric_translation(self):
        T=np.eye(4);t=np.array([.02,-.01,.7]);points=MODEL+t
        result=refine_optical(MODEL,None,np.eye(3),t+[.002,0,.003],[(T,view(points,T))])
        self.assertIsNotNone(result)
        np.testing.assert_allclose(result[1],t,atol=1e-6)

    def test_too_few_leds_do_not_invent_a_position(self):
        T=np.eye(4);t=np.array([0,0,.7]);points=MODEL+t
        self.assertIsNone(refine_optical(MODEL,None,np.eye(3),t,[(T,view(points[:2],T))]))

class ReacquisitionTests(unittest.TestCase):
    def test_global_search_with_disjoint_camera_leds(self):
        from translation_reacquisition import reacquire_translation
        TA=np.eye(4);TB=np.eye(4);TB[0,3]=.1
        t=np.array([.12,-.07,.63]);points=MODEL+t
        result=reacquire_translation(MODEL,None,np.eye(3),[(TA,view(points[:3],TA)),(TB,view(points[3:],TB))])
        self.assertIsNotNone(result)
        np.testing.assert_allclose(result[1],t,atol=1e-5)

    def test_parallel_rays_cannot_constrain_depth(self):
        from translation_reacquisition import reacquire_translation
        self.assertIsNone(reacquire_translation(MODEL,None,np.eye(3),[(np.eye(4),np.tile([0.,0.,1.],(6,1)))]))

    def test_behind_camera_jacobian_returns_missing(self):
        from gyro_optical import residual_jacobian
        r,J=residual_jacobian(MODEL,np.eye(3),np.array([0,0,-1]),[(np.eye(4),np.array([[0.,0.,1.]]),[(0,0)])])
        self.assertIsNone(r)
        self.assertIsNone(J)

class ConfirmationTests(unittest.TestCase):
    def test_single_false_pose_does_not_get_published(self):
        from translation_reacquisition import TranslationConfirmation
        check=TranslationConfirmation()
        self.assertFalse(check.observe([0,0,.7],1.))
        self.assertFalse(check.observe([.6,0,.7],1.04))
        self.assertFalse(check.observe([0,0,.7],1.08))
        self.assertFalse(check.observe([.01,0,.7],1.12))
        self.assertTrue(check.observe([.02,0,.7],1.16))

    def test_impossible_motion_and_gaps_reset_confirmation(self):
        from translation_reacquisition import TranslationConfirmation
        check=TranslationConfirmation()
        for ts in (1.01,1.02,1.03):
            self.assertFalse(check.observe([1,0,.7],ts,np.array([0,0,.7]),1.))
        self.assertFalse(check.observe([0,0,.7],2.))
        self.assertFalse(check.observe([0,0,.7],2.2))

class RobustOpticalTests(unittest.TestCase):
    def test_inconsistent_blob_is_removed_before_final_pose(self):
        import cv2
        TA=np.eye(4);TB=np.eye(4);TB[0,3]=.1
        t=np.array([.02,-.01,.7]);R=cv2.Rodrigues(np.array([.1,-.08,.04]))[0]
        points=MODEL@R.T+t
        a=view(points,TA);b=view(points,TB)
        # One reflection overlaps an expected LED neighborhood, but does not
        # obey the constellation geometry. Other views still see real LEDs.
        a[0,:2]+=[.012,-.012];a[0]/=np.linalg.norm(a[0])
        result=refine_optical(MODEL,None,R,t+[.002,-.001,.003],[(TA,a),(TB,b)])
        self.assertIsNotNone(result)
        np.testing.assert_allclose(result[1],t,atol=.001)
        np.testing.assert_allclose(result[0],R,atol=.002)

class HeadMotionOpticalTests(unittest.TestCase):
    def test_rotating_head_does_not_rotate_stationary_controller_in_world(self):
        import cv2
        from gyro_hypotheses import relative_prediction
        TA=np.eye(4);TB=np.eye(4);TB[0,3]=.1
        old_R=cv2.Rodrigues(np.array([.1,-.08,.04]))[0]
        old_t=np.array([.02,-.01,.7])
        head_delta=cv2.Rodrigues(np.array([.03,.12,-.02]))[0]
        expected_R=head_delta.T@old_R;expected_t=head_delta.T@old_t
        predicted_R=relative_prediction(old_R,np.eye(3),head_delta)
        points=MODEL@expected_R.T+expected_t
        result=refine_optical(MODEL,None,predicted_R,head_delta.T@old_t,[(TA,view(points,TA)),(TB,view(points,TB))])
        self.assertIsNotNone(result)
        np.testing.assert_allclose(head_delta@result[0],old_R,atol=1e-6)
        np.testing.assert_allclose(head_delta@result[1],old_t,atol=1e-6)
