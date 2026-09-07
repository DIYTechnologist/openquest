#!/usr/bin/env python3
"""ctl_decode.py — decode Quest 1 controller input from the raw SyncBoss stream.

No Meta userspace code is involved. Input is a capture of /dev/syncboss_stream0 (see sb_leech.c in
this directory), which any process can read passively — the device is a multi-reader broadcast
fifo, not single-open (research-notes/30).

Framing (research-notes/27, offset corrected in research-notes/49):

    01 03 00 <type> 00 <len> <payload>          outer syncboss framing
    type 0x8f = controller, ~500 Hz per controller

    payload[0:8]    device id
    payload[8:23]   15-byte header      <-- notes/27 said 14; it is 15
    payload[23:]    tagged records, each <tag> 0x80 <data[n]>, n fixed per tag
                    except the IMU record, which is 0x41 0x82 + 18 bytes

Control mapping established in notes/49 by pressing a known sequence on each controller and
matching each field's first deviation from idle to its position in that order — 9 events per
button, self-checked by a 3-short / 3-long / 3-short signature.
"""
import argparse
import collections
import struct
import sys

# ---------------------------------------------------------------- record sizes
REC_SIZE = {0x24: 1, 0x25: 1, 0x87: 4, 0x82: 4, 0x63: 3, 0x26: 1,
            0x23: 1, 0x22: 1, 0x21: 1}

# ---------------------------------------------------------------- button bits (0x24)
# Identical on both controllers: 0x01/0x02 are the primary/secondary face buttons, and whether they
# read A/B or X/Y is purely handedness. Verified on both controllers independently (notes/49).
BTN_PRIMARY = 0x01      # A (right) / X (left)
BTN_SECONDARY = 0x02    # B (right) / Y (left)
BTN_STICK = 0x04        # thumbstick click
BTN_SPECIAL = 0x08      # Oculus / menu

# ---------------------------------------------------------------- handedness
#
# HANDEDNESS MUST BE DERIVED, NOT HARDCODED. Device ids are per-unit: the ids observed on this
# headset (0143858a3ac372bd right, 37b8c4d954a7596e left) will be different on anyone else's, so a
# released decoder cannot key off them.
#
# Two header bytes qualify. Both are perfectly constant per controller and differ between them,
# across two independent capture sessions and ~334k packets, and both also appear in the 0xd9
# announce packet:
#
#     byte10 (header offset 2):   1 = right, 0 = left      <-- USED HERE
#     byte12 (header offset 4):   8 = right, 9 = left      <-- ALTERNATIVE, see below
#
# byte10 is preferred because it is boolean, which is the shape a handedness flag should have.
# byte12 (8 vs 9) looks more like a pairing slot or radio index than a semantic role.
#
# CAVEAT, and the reason byte12 is documented rather than discarded: with only one pair of
# controllers available, a genuine handedness flag and a per-device constant that merely correlates
# with handedness are indistinguishable. byte10 could be pairing-slot parity — the second controller
# paired gets 0. If a user ever reports swapped hands, that is the symptom, and the fix is to try
# HAND_FIELD = 'byte12' below.
#
# Falsifiable test to settle it properly: unpair and re-pair the controllers in the opposite order.
# If byte10 follows the physical controller it is handedness; if it follows the pairing order it is
# a slot index and the real signal is elsewhere. A second pair of controllers settles it equally.
HAND_FIELD = 'byte10'


def handedness(payload):
    """-> 'right' | 'left' | None, derived from the packet, never from the device id."""
    if HAND_FIELD == 'byte10':
        v = payload[10]
        return 'right' if v == 1 else 'left' if v == 0 else None
    if HAND_FIELD == 'byte12':
        v = payload[12]
        return 'right' if v == 8 else 'left' if v == 9 else None
    raise ValueError(f'unknown HAND_FIELD {HAND_FIELD!r}')


def iter_packets(blob):
    """Yield (type, payload) from a raw syncboss stream capture."""
    i, n = 0, len(blob)
    while i + 6 <= n:
        if not (blob[i] == 1 and blob[i+1] == 3 and blob[i+2] == 0 and blob[i+4] == 0):
            i += 1
            continue
        t, ln = blob[i+3], blob[i+5]
        if i + 6 + ln > n:
            break
        yield t, blob[i+6:i+6+ln]
        i += 6 + ln


def parse_records(payload):
    """-> {tag: data} for one controller packet. IMU records are skipped."""
    out, j = {}, 23
    while j + 2 <= len(payload):
        tag = payload[j]
        if tag == 0x41 and payload[j+1] == 0x82:     # IMU sub-record, 18 B payload
            j += 20
            continue
        if payload[j+1] != 0x80 or tag not in REC_SIZE:
            break                                    # unknown tail; stop rather than guess
        size = REC_SIZE[tag]
        out[tag] = payload[j+2:j+2+size]
        j += 2 + size
    return out


class ControllerState:
    """Decoded state of one controller."""

    __slots__ = ('device_id', 'hand', 'primary', 'secondary', 'stick_click', 'special',
                 'trigger', 'grip', 'stick_x', 'stick_y')

    def __init__(self, device_id, hand):
        self.device_id, self.hand = device_id, hand
        self.primary = self.secondary = self.stick_click = self.special = False
        self.trigger = self.grip = 0.0
        self.stick_x = self.stick_y = 0.0

    def label(self, which):
        """Face-button names depend on handedness, not on a different field layout."""
        if which == 'primary':
            return 'A' if self.hand == 'right' else 'X'
        return 'B' if self.hand == 'right' else 'Y'

    def update(self, recs):
        if 0x24 in recs:
            b = recs[0x24][0]
            self.primary = bool(b & BTN_PRIMARY)
            self.secondary = bool(b & BTN_SECONDARY)
            self.stick_click = bool(b & BTN_STICK)
            self.special = bool(b & BTN_SPECIAL)
        if 0x63 in recs:
            # Two 12-bit axes packed little-endian into 3 bytes, INVERTED: 0xFFF released, 0 fully
            # pressed. Normalised here to 0.0 released .. 1.0 pressed.
            raw = int.from_bytes(recs[0x63], 'little')
            self.trigger = 1.0 - (raw & 0xFFF) / 4095.0
            self.grip = 1.0 - ((raw >> 12) & 0xFFF) / 4095.0
        if 0x82 in recs:
            x, y = struct.unpack('<2h', recs[0x82])
            self.stick_x, self.stick_y = x / 32768.0, y / 32768.0

    def __str__(self):
        btns = [n for n, v in ((self.label('primary'), self.primary),
                               (self.label('secondary'), self.secondary),
                               ('STICK', self.stick_click), ('SPECIAL', self.special)) if v]
        return (f"{self.hand:5s} {self.device_id}  trig {self.trigger:4.2f}  grip {self.grip:4.2f}  "
                f"stick ({self.stick_x:+5.2f},{self.stick_y:+5.2f})  {' '.join(btns)}")


def decode(blob):
    """-> (states, events). Events are (packet_index, hand, name, is_down)."""
    states, events, idx = {}, [], 0
    for t, p in iter_packets(blob):
        if t != 0x8f or len(p) < 23:
            continue
        did = p[:8].hex()
        hand = handedness(p)
        st = states.get(did)
        if st is None:
            st = states[did] = ControllerState(did, hand)
        prev = (st.primary, st.secondary, st.stick_click, st.special)
        st.update(parse_records(p))
        now = (st.primary, st.secondary, st.stick_click, st.special)
        for name, a, b in zip((st.label('primary'), st.label('secondary'), 'STICK', 'SPECIAL'),
                              prev, now):
            if a != b:
                events.append((idx, st.hand, name, b))
        idx += 1
    return states, events


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('capture', help='raw /dev/syncboss_stream0 capture')
    ap.add_argument('--events', action='store_true', help='list button transitions')
    a = ap.parse_args()
    blob = open(a.capture, 'rb').read()
    states, events = decode(blob)
    print(f"controllers seen: {len(states)}")
    for st in states.values():
        print(f"  {st}")
    downs = collections.Counter((h, n) for _, h, n, d in events if d)
    print(f"\nbutton press counts ({sum(downs.values())} presses total):")
    for (h, n), c in sorted(downs.items()):
        print(f"  {h:5s} {n:8s} {c}")
    if a.events:
        print("\ntransitions:")
        for i, h, n, d in events:
            print(f"  pkt {i:7d}  {h:5s} {n:8s} {'DOWN' if d else 'up'}")


if __name__ == '__main__':
    sys.exit(main())
