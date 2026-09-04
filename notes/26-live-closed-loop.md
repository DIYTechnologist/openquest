# Live closed loop: our cameras -> our VIO -> Meta's compositor — 2026-09-04

Step 4 (`notes/18`). `notes/23` proved injection works and that the compositor renders from it, but
with a *recorded* trajectory. This is the estimator running on-device against the camera stream.

## Shape

Two processes joined by a pipe — capture is C against the msm camera ABI, estimation is C++ against
OpenVINS. ~18.5 MB/s through the pipe is cheap next to the estimator's per-frame cost.

```
cam_kernel 4 <secs> 3000 160 02 -  |  vio_live <config.yaml> <imu_rect.txt> [--no-inject]
```

`cam_kernel` gained a stream mode (`outdir "-"`) emitting typed records: `I` imu, `E` exposure
stamp, `C` frame. Every record carries **both** the device clock and the host monotonic time it was
observed, because the two clocks are not the same one.

## The clock problem, and what it actually took

Three timelines, and getting them wrong produces *silent* failure — no error, just an estimator that
never initialises or one that diverges.

1. **Camera V4L2 stamps are `CLOCK_MONOTONIC`. The IMU is on the nRF clock.** Measured epoch gap:
   **4318 s** on this capture. The offline builder hides this behind a whole-capture least-squares
   fit, which a live consumer cannot do.
2. **`0xe0` is the bridge** — it carries the camera exposure timestamp *on the nRF clock*. Decoding
   it required care: **the u32 is at payload offset 1, not 0.** Read at 0 it gives 8533 ms deltas
   (the low byte is a tag); read at 1 it gives exactly **33333 us — 30 Hz**. This is what the
   offline builder's "cam shift 816 ms" was compensating for.
3. **Everything runs on the nRF clock.** The IMU already is; frames are mapped onto it with a
   constant offset estimated as a **median** of (V4L2 stamp − most recent exposure stamp) over a
   sliding window. Median, not one-to-one association: a single dropped frame would desync a
   positional pairing permanently, whereas the offset is genuinely constant and robustly estimable.

Two bugs found on the way here, both instructive because neither reported an error:

- **Naive first-sample alignment** (align first IMU to first frame) → the estimator initialised and
  then **diverged to 1109, −2504, −1276 m**. Time misalignment does not look like a timing bug; it
  looks like a broken estimator.
- **Mapping the IMU to `CLOCK_MONOTONIC` while frames stayed on nRF** → the two streams sat 4318 s
  apart and the estimator simply **never initialised**. No error, no divergence, just silence.

## Validating without a person

A live desk capture cannot validate any of this: OpenVINS gates initialisation on IMU excitation, so
a stationary headset never initialises and proves nothing. Confirmed — a 20 s live run produced
19847 IMU samples, 388 stereo pairs, and `tracked 0.00 s`.

`tools/vio_live/replay_feed` replays a recorded capture **as if it were live**, reconstructing real
host arrival times from `syncboss_chunks.csv` so the consumer's clock logic is genuinely exercised
rather than handed a synthetic timeline that would hide exactly the bugs above. Replaying the 75 s
motion capture from `notes/22`:

```
[replay] 76645 syncboss chunks, 4492 frames, 74388 imu samples, 2256 exposure stamps
[+] camera->IMU clock offset = 4318.658 s
[+] initialised after 1892 frames / 63317 imu
[+] imu=74388 expo=2256 frames=4486 pairs=2227 injected=336 failed=0
[+] estimator median 17.84 ms/frame (p90 67.19), tracked 11.17 s
```

And Meta's tracker, driven by it: `pos=(-0.0576, +0.0710, -0.1038) valid=True`.

**Bounded, metre-scale, no divergence** — the whole chain runs.

## Honest gaps

- **Not yet run fully live with motion.** Capture-live and estimate-on-replay are each validated;
  the two have not been run together against real movement, which needs someone to move the headset.
- **The live consumer does not reproduce the offline result exactly, and I have not found out why.**
  Offline: initialised 53.9 s in (~1611 pairs), tracked 21.1 s, final ‖p‖ 0.203 m. Live-path replay:
  initialised ~63 s in (1892 pairs), tracked 11.17 s. Same order, bounded, but ~9 s later to
  initialise.

  Three candidate mechanisms were proposed and **all three are disproved by measurement**:

  | hypothesis | measured | verdict |
  |---|---|---|
  | Insufficient IMU lead (offline uses `IMU_LEAD_S` = 0.10 s) | live lead median **23.6 ms**, negative on 0.2 % | not the cause — lead is positive and sufficient |
  | Timestamp jitter from a constant V4L2→exposure offset instead of per-frame snapping | offset spread **±0.02 ms**, full range 0.08 ms | not the cause — the offset really is constant |
  | IMU delivered out of order by bursty chunk arrival, silently discarded | **0 / 74388 out of order**; 46 older than the last frame | not the cause |

  So the feed into the estimator is clean. The leading remaining hypothesis is that the **on-device
  OpenVINS build differs from the docker one** — notably a different OpenCV, which changes feature
  detection and therefore how quickly initialisation accumulates enough disparity. That is testable
  by running the on-device `ov_bench` against a EuRoC dataset on the device and comparing with the
  host result, which needs the ~1.1 GB dataset pushed; not done.

  **Recorded as open.** No accuracy claim should be attached to the live path until it is closed.
- **p90 is 67 ms** against a 33.3 ms budget, well above the 31.75 ms median measured in `notes/20`.
  Frame drops under load are likely, and `notes/20`'s caveat — that the median fits but the tail
  does not — now has teeth.
