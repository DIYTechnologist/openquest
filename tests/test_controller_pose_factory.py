import json
import struct
import sys
import unittest
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'tools/controller_tracking'))
from factory_controller_model import import_model


def calibration():
    points={f'Point{i}':[i*.01,0,.02,0,0,1,85,80,0] for i in [3,1,0,2]}
    root={'DeviceSecret':'not exported','TrackedObject':{'ModelPoints':points,'ImuPosition':[.001,.002,.003],'GyroCalibration':[1,0,0,0,1,0,0,0,1,0,0,0],'JsonVersion':3}}
    raw=json.dumps(root).encode();return struct.pack('<Q',len(raw))+raw


class FactoryModelTests(unittest.TestCase):
    def test_length_header_led_order_geometry_and_privacy(self):
        model=import_model(calibration())
        self.assertEqual(model['led_ids'],[0,1,2,3])
        self.assertEqual(model['imu_position_model'],[.001,.002,.003])
        self.assertNotIn('DeviceSecret',json.dumps(model))
        self.assertEqual(len(model['led_normals']),4)

    def test_truncated_and_wrong_lengths_are_rejected(self):
        for raw in (b'',calibration()[:-1],b'\0'*8+calibration()[8:]):
            with self.assertRaises(ValueError):import_model(raw)

    def test_invalid_normal_is_rejected(self):
        root=json.loads(calibration()[8:]);root['TrackedObject']['ModelPoints']['Point0'][3:6]=[0,0,0]
        raw=json.dumps(root).encode()
        with self.assertRaises(ValueError):import_model(struct.pack('<Q',len(raw))+raw)
