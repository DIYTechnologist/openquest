# Controller optical / inertial replay prototype

This is experimental controller-tracking research, not a complete controller runtime or
LineageOS port. It uses camera bearings, a controller LED model and raw controller gyro
samples to estimate model-to-headset poses. Meta reference poses are used for evaluation
and separate reference-frame calibration, never as an estimator input.

## Implemented

- Import all 15 factory LED positions and outward normals from the per-controller
  calibration file (`factory_controller_model.py`). The file has an eight-byte little-endian
  JSON-length prefix. Keep the original file and unit identifiers private.
- Apply factory root `gyro_b` subtraction after unit conversion, then row-major `gyro_m`.
  Gyro scale and receipt-clock correction are currently measured calibration inputs.
- Initialize / reacquire from rigid, metric stereo constellation registration. No scale fit.
- Split saturated LED cores while preserving separate dim emitters. This avoids merging
  nearby bright LEDs into a single incorrectly located point.
- Use gyro-predicted orientation and optical translation / orientation refinement. Different
  views may observe different LEDs. Tighten association gates over three refinement passes
  to remove inconsistent blobs before accepting the pose.
- Use calibrated additional camera pairs for reacquisition when the primary pair fails.
- Optionally compensate headset rotation with its separately calibrated gyro. Transport
  predicted position and velocity into the current headset frame. This does not replace
  positional head VIO or provide a world-space controller API.
- Leave unobserved positions missing. Gyro propagation does not create an optical measurement.

`translation_reacquisition.py` additionally searches translation with known gyro orientation
and independent camera LED sets. Its bounded search rejects ambiguous alternatives and
requires three consecutive, speed-bounded candidates. This remains optional: on current
recordings it adds substantial cost without a reliable coverage improvement.

## Replay

Run with Python, NumPy and OpenCV. Camera calibration is the Basalt JSON generated from the
headset factory calibration. Capture inputs are `frames.bin` and `frames.idx` in the existing
`bootstrap_model.py` format. All camera timestamps must share the host monotonic clock.
Controller and head gyro JSONL files are produced by `controller_imu.py` and `head_imu.py`
from a passive SyncBoss recording plus its receipt index.

```sh
python tools/controller_tracking/inertial_optical_track.py \
    CAPTURE CAMERA_CALIBRATION FACTORY_MODEL CONTROLLER_IMU GYRO_CALIBRATION OUTPUT \
    --cameras 0 1 2 3 --min-peak-intensity 80 --split-bright-blobs \
    --head-imu HEAD_IMU --diagnostics DIAGNOSTICS
```

The brightness cutoff is an explicit experiment parameter, not a universal device setting.
Clock offsets use only the first half-second of receipt samples. The estimator waits for
that prefix before initialization. Receipt timestamps are not exposure timestamps; transport
latency and inter-sensor clock synchronization remain limitations. Additional camera frames
must be within 100 microseconds of the camera-0 anchor. Initialization still needs at least
four common LEDs in one calibrated stereo pair.

CSV rows contain capture timestamp, a reserved field, distinct LED support, normalized
reprojection error, method, model-centroid translation and quaternion (x, y, z, w). Empty pose
fields mean missing tracking. This is not the controller IMU origin. Comparing against another
tracker requires a fixed, independently calibrated origin and axis transformation.

## Development measurements

These recordings have been used for diagnosis and development. They are not final independent
accuracy validation. Headset was physically stationary for the measurements below.

| Recording | Earlier replay | Current robust replay | Notes |
| --- | ---: | ---: | --- |
| Long controller motion, 2 cameras | 727/1002 | 985/1002 | 12-frame warmup plus five missing frames |
| Short motion after still lead-in, 2 cameras | 325/352 | 340/352 | Mostly stationary; insufficient alone |
| New 30-second motion, 4 cameras | 548/752 using primary pair | 740/752 using all views / pairs | All 740 frames after initialization solved |

For the last row, fixed reference calibration from an earlier recording gives 9.7 mm position
RMSE and 24.3 mm maximum position difference on timestamp-matched emitted poses. Rotation
difference median is 3.9 degrees and maximum 12.8 degrees. The older long controller-motion
recording gives 10.3 mm RMSE, with a 62.4 mm worst position difference that still needs diagnosis.
These comparisons include reference-frame/calibration uncertainty and omit missing estimates;
coverage must be reported alongside errors.

An additional headset-rotation test (stationary controller, 23-degree headset rotation and
approximately 2.4 cm headset translation range) initially produced no poses with the frozen
low-threshold detector. Inspection exposed merged LED blobs. The split-core detector then
produced 740/753 poses, all frames after initialization. Fixed earlier reference calibration
reported 14.0 mm RMSE, 16.2 mm worst position difference and 5.9-degree worst orientation
difference. This recording became development data once the detector was corrected on it.
The controller reference was exactly stationary in this recording; its tracking-mode semantics
and possible stationary locking must be considered when interpreting its precision.

Processing-time experiments on the development Mac are not Android live latency or
motion-to-photon measurements. Concurrent replay jobs also distort timing comparisons.

## Still missing

- Independent moving-head accuracy and loss / recovery validation.
- Reliable recovery from sparse / edge-on LED observations and larger motion.
- Verified controller gyro scale from device configuration rather than empirical calibration.
- Native Android processing, bounded live transport and runtime pose / validity interface.
- Integration with positional headset VIO and an open compositor / OpenXR runtime.
- Sustained on-device performance, latency and application tests.

Synthetic geometry, sensor decoding, timing, outlier and head-rotation tests live in
`tests/test_controller_pose*.py`. They establish mathematical / parsing behavior, not hardware
tracking accuracy. Do not include raw camera recordings, controller calibration files or unit
identifiers in a public contribution.

## Independent combined-motion check

After the split-core fix, estimator sources and earlier reference calibration were frozen
and hashed before a new 20-second recording. Both headset and right controller moved:
headset rotation reached 32.5 degrees, headset position ranges reached 7.8 cm, and controller
position ranges reached 28.1 cm. The frozen estimator produced 491/503 poses, with the first
12 frames used for initialization and no missing poses afterward. On 487 timestamp-matched
reference pairs, fixed earlier reference calibration gave 13.1 mm position RMSE, 25.1 mm worst
position difference, 4.46-degree median and 7.69-degree maximum orientation difference.
No estimator parameters or reference alignment were fitted to this capture.

This validates one short recorded combined-motion sequence. It does not establish robustness
to occlusion, fast motion, lighting changes or prolonged use. Processing was on the development
Mac (6.6 ms median, 7.9 ms p99), not a native Android/live runtime. Raw recordings and per-unit
calibration remain private.
