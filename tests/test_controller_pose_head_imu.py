import sys
import unittest
import struct
from pathlib import Path
import numpy as np
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'tools/controller_tracking'))
from head_imu import decode_head


class HeadImuTests(unittest.TestCase):
    def test_units_are_converted_before_factory_bias_and_rectification(self):
        matrix=np.array([[0,-1,0],[-1,0,0],[0,0,-1]])
        c={'ImuCalibration':{'Gyroscope':{'RectificationMatrix':matrix.ravel().tolist(),'Offset':{'ConstantOffset':[.1,.2,.3]}}}}
        payload=struct.pack('<II7f',123,0,0,0,1,180,0,0,20)
        result=decode_head(payload,c)
        np.testing.assert_allclose(result['omega_rad_s'],matrix@np.array([np.pi-.1,-.2,-.3]))
        self.assertEqual(result['sensor_us'],123)

    def test_invalid_payload_is_not_a_measurement(self):
        self.assertIsNone(decode_head(b'bad',{}))
