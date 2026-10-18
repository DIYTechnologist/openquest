import sys
import unittest
from pathlib import Path
import numpy as np
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'tools/controller_tracking'))
from blob_detect import find_blobs,find_led_blobs

class BlobTests(unittest.TestCase):
    def test_merged_halos_yield_two_separate_bright_leds(self):
        y,x=np.mgrid[:50,:80]
        im=np.clip(255*np.exp(-((x-30)**2+(y-25)**2)/18)+255*np.exp(-((x-42)**2+(y-25)**2)/18),0,255).astype(np.uint8)
        self.assertEqual(len(find_blobs(im,max_area=500)),1)
        result=find_led_blobs(im,max_area=500)
        self.assertEqual(len(result),2)
        np.testing.assert_allclose(sorted(result[:,0]),[30,42],atol=.2)

    def test_dim_led_is_retained_without_duplicate_bright_led(self):
        im=np.zeros((50,80),np.uint8);im[10:13,10:13]=100;im[30:33,30:33]=255
        result=find_led_blobs(im)
        self.assertEqual(len(result),2)
        self.assertEqual(sorted(result[:,3]),[100,255])

    def test_blank_image_does_not_produce_leds(self):
        self.assertEqual(find_led_blobs(np.zeros((40,40),np.uint8)).shape,(0,4))
