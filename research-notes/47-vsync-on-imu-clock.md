# 0x55 is display vsync on the IMU clock — the motion-to-photon instrument — 2026-09-05

`notes/45` noticed that syncboss stream type 0x55 runs at 71.9 Hz, matching the panel refresh
measured independently in `notes/44`. A matching rate is suggestive, not proof — two independent
oscillators can sit at similar frequencies. Tested properly, and it is proof.

## Phase-lock test

Captured simultaneously: 0x55 packets from `/dev/syncboss_stream0` (nRF 1 MHz clock) and
`/sys/class/graphics/fb0/vsync_event` (CLOCK_MONOTONIC), over ~14 s.

Index-pairing the two sets is invalid — each `cat vsync_event` blocks for the next event, so reads
between `cat` invocations are lost (200 sysfs reads against 1006 0x55 events). The correct test fits
a periodic grid to 0x55 and asks where the sysfs events fall on it, searching over the unknown
constant clock offset:

```
0x55 grid fit   period 13.923525 ms, residual sd 0.29 us   -> 71.8209 Hz
fb vsync events on that grid: phase sd 16.9 us, spread 66.1 us
random phase would give sd ~4019 us
```

**240x tighter than random.** They are the same physical event.

## Why this matters

Three things now sit on **one clock**, the nRF 1 MHz timebase, readable losslessly from a single
passive reader (`notes/30`) with no interposition anywhere:

| signal | type | rate |
|---|---|---|
| headset IMU | 0x50 | 993.6 Hz |
| camera exposure | 0xe0 | 25 Hz per exposure class |
| **display vsync** | **0x55** | **71.8209 Hz** |

That is the entire sensor-to-photon chain timed on a common clock without touching the display
stack, without a photodiode, and without cross-clock correlation. `notes/18` step 6 task 1 asks for
motion-to-photon "with the method stated" — the method is now available, and it needs no hardware
this project does not have.

It is also a better instrument than the display's own interface: 0x55 has **0.29 us** grid residual
against 5.2 us for `vsync_event` read through sysfs, and it does not drop events.

## Refined refresh figure

The grid fit supersedes `notes/44`'s 71.819 Hz with a much tighter estimate:

```
period 13.923525 ms   ->   71.8209 Hz     (residual sd 0.29 us)
nominal 72 Hz = 13.888889 ms  ->  +34.6 us per frame
```

Unchanged in substance — the panel runs 0.18 Hz below nominal — but now good to ~0.3 us per frame
rather than ~3 us.

## What is still missing for the number

The remaining unknown is which *displayed frame* a given injected pose lands in. Timing is solved;
attribution is not. Candidate approach: inject a pose step (`notes/23` proved the compositor renders
from injected poses) and find the vsync at which the rendered output changes — which still needs a
way to observe the output. Recorded as the open half rather than glossed.
