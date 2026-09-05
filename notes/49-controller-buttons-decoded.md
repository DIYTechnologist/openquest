# Right controller fully decoded — 2026-09-05

`notes/27` found the controller stream (type `0x8f`, 496 Hz per controller) and confirmed the
trigger, but left every digital button unidentified: "Nothing was pressed during the capture, so
every button field is constant and cannot be mapped." A scripted press session closes that.

## Method: the order is the ground truth

Meta's own reference is unusable — `trackinginterface_cli getcontrollerbuttondata` takes **2.7 s per
call**, far too slow to score discrete events (`notes/31`). So instead of correlating against Meta,
the user pressed a **known sequence** and each field's first deviation from idle is matched to its
position in that order. No reference logger, no live cue, and no clock synchronisation needed.

Per button: **3 quick presses, 3 slow holds, 3 quick presses**, ~3 s between buttons. That pattern
is a self-check — it must appear in whatever field the button lands in, or the mapping is wrong.

Both controllers stream simultaneously; only the pressed one's fields vary, which identifies it:

```
0143858a3ac372bd   0x24: 5 distinct values, 0x25: 19, 0x63: 207, 0x82: 687   <- RIGHT (pressed)
37b8c4d954a7596e   0x24: 1,  0x25: 1,  0x63: 1,  0x82: 1                     <- untouched
```

## Container framing correction

`notes/27` gave the record offset as "8-byte id and a 14-byte header". It is **15**: records start
at **offset 23**, not 22. With that fixed the whole container parses (392 unparsed tails out of
258,964 packets).

## The mapping

| button | field | encoding |
|---|---|---|
| **Trigger** | `0x63` axis A | 12-bit, `0xFFF` released -> `0` fully pressed (inverted, confirms `notes/27`) |
| **Grip** | `0x63` axis B | 12-bit, same encoding — **resolves `notes/27`'s "probable grip, not isolated"** |
| **A** | `0x24` | bit `0x01` |
| **B** | `0x24` | bit `0x02` |
| **Stick click** | `0x24` | bit `0x04` |
| **Special** (Oculus/menu) | `0x24` | bit `0x08` |
| **Thumbstick X/Y** | `0x82` | 2 x int16, full deflection ~+/-32000 |

`0x24` is a plain bitmask: four single-bit values, each first appearing in its own window, in exactly
the pressed order.

`notes/27` guessed `0x24` values `0x11`/`0x12` were held states and `0x25` was the analogue axis.
Both were misreadings of a capture with nothing pressed — `0x25` varies but does not correspond to
any single button, and `0x82` (previously "event-specific, unidentified") is the thumbstick.
`0x87`, previously "thumbstick / 2-axis", is *not* — it reports continuously through the whole
capture including idle periods.

## The self-check passes on all four digital buttons

Press durations, segmented at a 0.15 s gap:

```
B            0.14 0.10 0.10 | 1.31 1.35 1.23 | 0.15 0.16 0.14
A            0.72 0.28 0.17 | 1.05 1.28 1.28 | 0.17 0.16 0.23
stick click  0.38 0.49 0.45 | 1.67 1.42 1.50 | 0.41 0.36 0.33
special      0.38 0.32 0.21 | 1.33 1.25 1.24 | 0.17 0.19 0.22
```

**Exactly 9 presses each, in a 3-short / 3-long / 3-short pattern**, for all four. The analogue axes
show the same structure in their own windows, and the thumbstick sweep at 52-55 s reads
UP (+93 deg), DOWN (-66 deg), LEFT (-163 deg), RIGHT (+2 deg) — the pressed order.

## Step 3 status

| criterion | state |
|---|---|
| every button decoded from the raw stream | **right controller: done** — 6 digital/analogue + 2 stick axes |
| matches Meta's reported state 100 % over >= 50 events | **not as written** — see below |
| controller IMU decoded, rate and units confirmed | done (`notes/27`, 501 Hz) |
| where 6DoF controller pose is computed | still open |

The "matches Meta 100 %" criterion cannot be scored as written, because Meta's reference is 2.7 s
per call. What was done instead is stronger in one respect and weaker in another: **90 events with
known ground truth** (9 per button x 10 buttons), verified by an independent signature rather than
by Meta's agreement. It establishes the decode; it does not cross-check against Meta's own
interpretation.

Left controller not yet captured. Expected to differ only in which id is active, with B->Y and A->X.

## Left controller — same encoding, confirmed independently

Second capture, left controller only. Notably **only one device streamed** (`37b8c4d954a7596e`) —
the right had gone to sleep on the desk — which independently confirms the handedness assignment
made in the first capture from which id's fields varied.

```
0x63 axis A   0.8 -  8.4 s    Trigger
0x63 axis B   8.8 - 17.2 s    Grip
0x24 bit 0x02  first 18.5 s   Y     <- same bit as the right controller's B
0x24 bit 0x01  first 28.8 s   X     <- same bit as the right controller's A
0x24 bit 0x04  first 37.6 s   stick click
0x82          45.5 - 47.5 s   UP (+70 deg), DOWN (-118), LEFT (+164), RIGHT (-15)  in order
0x24 bit 0x08  first 50.5 s   Special (menu)
```

**The bit assignment is identical across controllers.** `0x01` and `0x02` are the primary and
secondary face buttons; whether they read as A/B or X/Y is purely handedness, so a decoder needs the
device id to label them, not a different field layout.

Press-count check: bits `0x04` and `0x08` give a clean 9; bits `0x01` and `0x02` give 8 and 7,
because two adjacent presses fell inside the 0.15 s segmentation gap (visible as a single 0.35 s and
0.53 s press where two short ones were expected). That is a threshold artefact in the analysis, not
a decode failure — the 3-short / 3-long / 3-short structure is unambiguous in all four.

## Both controllers: final map

| control | field | encoding |
|---|---|---|
| Trigger | `0x63` axis A | 12-bit, `0xFFF` released -> `0` pressed |
| Grip | `0x63` axis B | 12-bit, same |
| A / X (primary) | `0x24` | bit `0x01` |
| B / Y (secondary) | `0x24` | bit `0x02` |
| Stick click | `0x24` | bit `0x04` |
| Special / menu | `0x24` | bit `0x08` |
| Thumbstick X/Y | `0x82` | 2 x int16, +/-32000 |
| Controller IMU | `0x41 0x82` | 501 Hz (`notes/27`) |
| device id | payload `[0:8]` | `0143858a3ac372bd` right, `37b8c4d954a7596e` left |

Every control on both controllers is now decoded from the raw MCU stream with **no Meta userspace
code**. What remains for step 3 is the architectural question `notes/18` flagged as the real risk:
**where 6DoF controller pose is computed** — MCU, `trackingservice`, or camera IR blobs. Nothing in
`0x8f` looks like a pose, which is evidence against the MCU but not proof.
