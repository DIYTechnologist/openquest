"""Metric stereo registration synthetic tests; not camera accuracy measurements."""
import sys
import unittest
from pathlib import Path
import numpy as np
import cv2
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'tools/controller_tracking'))
from stereo_registration import rigid_fit,stereo_cloud,register,refine_stereo

MODEL=np.array([[-.04,-.03,.002],[.035,-.025,-.005],[.045,.025,.012],[-.012,.04,-.008],[-.03,.019,.015]])

class RegistrationTests(unittest.TestCase):
    def test_rigid_fit_does_not_introduce_scale_or_reflection(self):
        R=cv2.Rodrigues(np.array([.2,-.3,.1]))[0];t=np.array([.1,.02,.6])
        actualR,actualt=rigid_fit(MODEL,MODEL@R.T+t)
        np.testing.assert_allclose(actualR,R,atol=1e-12)
        np.testing.assert_allclose(actualt,t,atol=1e-12)
        self.assertAlmostEqual(np.linalg.det(actualR),1)

    def test_triangle_registration_recovers_permuted_cloud_with_outlier(self):
        R=cv2.Rodrigues(np.array([.2,-.3,.1]))[0];t=np.array([.1,.02,.6])
        cloud=np.vstack([(MODEL@R.T+t)[[3,0,4,1,2]],[[.4,.3,.8]]])
        result,_=register(MODEL,cloud,[(i,i) for i in range(6)])
        self.assertIsNotNone(result)
        np.testing.assert_allclose(result[0],R,atol=1e-10)
        np.testing.assert_allclose(result[1],t,atol=1e-10)
        self.assertEqual(len(result[3]),5)

    def test_reused_camera_identity_cannot_create_triangle(self):
        result,_=register(MODEL,MODEL+[0,0,.6],[(0,i) for i in range(5)])
        self.assertIsNone(result)

    def test_stereo_triangulation_recovers_true_metric_points(self):
        TA=np.eye(4);TB=np.eye(4);TB[0,3]=.1
        points=MODEL+[0,0,.6]
        da=points/np.linalg.norm(points,axis=1,keepdims=True)
        rays=points-TB[:3,3];db=rays/np.linalg.norm(rays,axis=1,keepdims=True)
        cloud,ids=stereo_cloud(da,db,TA,TB,epipolar_gate=1e-8)
        for i in range(len(MODEL)):
            j=ids.index((i,i));np.testing.assert_allclose(cloud[j],points[i],atol=1e-10)

    def test_joint_camera_refinement_reduces_known_pose_error(self):
        TA=np.eye(4);TB=np.eye(4);TB[0,3]=.1
        R=cv2.Rodrigues(np.array([.1,-.2,.15]))[0];t=np.array([.01,.02,.7]);points=MODEL@R.T+t
        da=points/np.linalg.norm(points,axis=1,keepdims=True)
        rays=points-TB[:3,3];db=rays/np.linalg.norm(rays,axis=1,keepdims=True)
        pairs=[(i,i) for i in range(5)]
        result=refine_stereo(MODEL,np.eye(3),t+[.005,-.003,.01],pairs,pairs,da,db,TA,TB)
        self.assertIsNotNone(result)
        np.testing.assert_allclose(result[0],R,atol=1e-7)
        np.testing.assert_allclose(result[1],t,atol=1e-7)

    def test_candidate_finalizer_can_refuse_all_hypotheses(self):
        pose,stats=register(MODEL,MODEL+[0,0,.6],[(i,i) for i in range(5)],finalize=lambda *args:None)
        self.assertIsNone(pose)
        self.assertGreater(stats['finalists'],0)

class JacobianTests(unittest.TestCase):
    def test_analytic_jacobian_matches_finite_differences(self):
        from stereo_registration import stereo_residual_jacobian
        R=cv2.Rodrigues(np.array([.2,-.3,.15]))[0];t=np.array([.03,-.01,.7])
        TA=np.eye(4);TB=np.eye(4);TB[:3,:3]=cv2.Rodrigues(np.array([.02,.1,-.03]))[0];TB[:3,3]=[.1,.01,0]
        observations=[np.tile([.1,.1,1.],(5,1)) for _ in range(2)]
        residual,J=stereo_residual_jacobian(MODEL,R,t,observations,(TA,TB))
        numerical=[]
        for i in range(6):
            d=np.zeros(6);d[i]=1e-7
            shifted,_=stereo_residual_jacobian(MODEL,cv2.Rodrigues(d[:3])[0]@R,t+d[3:],observations,(TA,TB))
            numerical.append((shifted-residual)/1e-7)
        np.testing.assert_allclose(J,np.array(numerical).T,atol=1e-6)
