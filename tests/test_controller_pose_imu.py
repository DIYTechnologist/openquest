import io
import struct
import sys
import unittest
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'tools/controller_tracking'))
from controller_imu import imu_records,timed_packets


def payload():
    header=bytearray(23);header[10]=1
    return bytes(header)+b'\x41\x82'+(12345678).to_bytes(6,'little')+struct.pack('<6h',1,-2,1000,-10,20,-30)


class ImuTests(unittest.TestCase):
    def test_exact_signed_fields_and_timestamp(self):
        sample=list(imu_records(payload()))[0]
        self.assertEqual(sample['sensor_us'],12345678)
        self.assertEqual(sample['accel_raw'],[1,-2,1000])
        self.assertEqual(sample['gyro_raw'],[-10,20,-30])
        self.assertEqual(sample['hand'],'right')

    def test_truncated_imu_never_exposed(self):
        self.assertEqual(list(imu_records(payload()[:-1])),[])

    def test_chunk_split_retains_completion_receipt(self):
        p=payload();raw=b'\x01\x03\x00\x8f\x00'+bytes([len(p)])+p
        index=io.StringIO(f'100,0,8\n200,8,{len(raw)-8}\n')
        packets=list(timed_packets(io.BytesIO(raw),index))
        self.assertEqual(packets,[(200,0x8f,p)])

    def test_invalid_receipt_index_is_rejected(self):
        with self.assertRaises(ValueError):list(timed_packets(io.BytesIO(b'abc'),io.StringIO('100,1,3\n')))
