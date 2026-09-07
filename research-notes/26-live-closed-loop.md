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

  So the feed into the estimator is clean. **RESOLVED 2026-09-04: it is not my consumer at all — it
  is the build.** Running the *on-device* `ov_bench` against the same EuRoC dataset, through the
  *offline* feeding path, initialises at the same late point:

  | run | build | OpenCV | feeding path | initialises at |
  |---|---|---|---|---|
  | host `euroc_runner` | docker | **4.5.4** | offline | ~1611 pairs |
  | on-device `ov_bench` | android | **4.10.0** | offline | **1800–2000 pairs** |
  | on-device `vio_live` | android | **4.10.0** | live stream | **1892 pairs** |

  `vio_live` matches the on-device offline runner, not the host. Same dataset, same config, same
  feeding logic — the only difference that tracks the result is the OpenCV version, which changes
  feature detection and therefore how quickly initialisation accumulates enough disparity.

  **Consequence worth carrying:** host and on-device results are **not directly comparable**. The
  step-1 acceptance figure (final ‖p‖ 0.203 m, `notes/22`) was produced by the host build; it
  remains valid as stated, but any future comparison must hold the build fixed.
- **p90 is 67 ms** against a 33.3 ms budget, well above the 31.75 ms median measured in `notes/20`.
  Frame drops under load are likely, and `notes/20`'s caveat — that the median fits but the tail
  does not — now has teeth.

---

# 10-minute injection soak — and a monitor that was killing the service

## The trap: `dumpsys | grep -m1` sends SIGPIPE

A 10-minute soak looping our VIO trajectory into the injection interface showed
**`trackingservice` restarting on almost every cycle** — 9 restarts in 10 cycles. The obvious
reading was that sustained injection destabilises it. That reading was wrong.

```
init: Service 'trackingservice' (pid 25070) received signal 13
init: updatable process 'trackingservice' exited 4 times in 4 minutes
init: starting service 'trackingservice'...
```

**Signal 13 is SIGPIPE.** The monitor ran `dumpsys tracking | grep -m1 ...`; `grep -m1` exits after
its first match, closing the pipe while `trackingservice` is still writing its dump. The service
takes SIGPIPE and init restarts it. **The monitoring command was killing the service it monitored.**

This also explains `Failed to write while dumping service tracking: Broken pipe`, which appears
throughout this session's logs and had been treated as cosmetic noise. It was not: **any earlier
observation taken immediately after a `dumpsys tracking | grep -m1` may reflect a service that had
just been killed and restarted** — including at least one `getHeadTrackingData` returning `{}` and
one unexplained drop to 3DOF. Those readings should not be trusted.

Safe form — consume the whole dump, then filter:
```sh
out=$(dumpsys tracking 2>/dev/null)
lvl=$(echo "$out" | grep -o "[0-9]DOF" | head -1)
```

## Result with the monitor fixed

| metric | result |
|---|---|
| duration | **607 s** (29 cycles) |
| poses injected | 29 x 621 = **~18,000 at 30 Hz** |
| injection failures | **0** (29/29 cycles reported `0 failed`) |
| `trackingservice` restarts | **0** |
| `vrshell` restarts | **0** |
| tracking level | **6DOF throughout** |

Sustained injection at frame rate is stable for at least ten minutes.

## A criterion in notes/18 names a process that does not exist

Step 4 asks for *"zero crashes of `com.oculus.systemdriver` over a 10-minute session"*.
**`com.oculus.systemdriver` does not run on this device.** The shell-side processes are
`com.oculus.shellenv`, `com.oculus.systemux:SystemUX` and `com.oculus.vrshell`. The soak monitored
`trackingservice` and `vrshell`; the criterion should be restated against those.

Still outstanding for step 4: *"Meta's shell renders and responds to head motion for >= 10 minutes"*
needs someone wearing the headset — stability is now shown, responsiveness is not.
