#!/usr/bin/env python3
"""Regression tests for bridge/quest_proto.py (stdlib only)."""
import math
import os
import struct
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import quest_proto as qp


def tracking_bytes(head=(0, 1.6, 0, 0, 0, 0, 1), hands=None, ts=123):
    eyes = b''.join(struct.pack('<7f', *(hx, 1.6, 0, 0, 0, 0, 1))
                    + struct.pack('<4f', -.8, .8, .8, -.8)
                    for hx in (-.032, .032))
    if hands is None:
        hands = [(qp.HAND_ACTIVE, 0), (0, 0)]
    out = [struct.pack('<Q', ts) + struct.pack('<7f', *head) + eyes]
    for flags, buttons in hands:
        out.append(struct.pack('<II', flags, buttons)
                   + struct.pack('<7f', 0, 0, 0, 0, 0, 0, 1) * 2
                   + struct.pack('<4f', 0, 0, 0, 0))
    body = b''.join(out)
    assert len(body) == 284, len(body)
    return body


class TrackingTest(unittest.TestCase):
    def test_round_trip(self):
        t = qp.parse_tracking(tracking_bytes(
            head=(0.1, 1.7, -0.2, 0, 0, 0, 1), ts=999))
        self.assertEqual(t['time_ns'], 999)
        self.assertAlmostEqual(t['head'][0], 0.1)
        self.assertAlmostEqual(t['head'][1], 1.7)
        self.assertEqual(t['hands'][0]['flags'], qp.HAND_ACTIVE)

    def test_short_rejected(self):
        with self.assertRaises(ValueError):
            qp.parse_tracking(b'\x00' * 100)


class ProfileTest(unittest.TestCase):
    def test_quest1_native_pixels(self):
        p = qp.device_profile({'device': 'Quest', 'eye_w': 1216,
                               'eye_h': 1344, 'refresh_rates': [72],
                               'codecs': ['hevc', 'h264']})
        self.assertEqual(p['device'], 'quest1')
        self.assertEqual(p['codec'], 'h264')
        self.assertEqual((p['eye_w'], p['eye_h']), (1216, 1344))
        self.assertEqual(p['eye_w'] * 2, 2432)  # full SBS frame

    def test_generic_echoes_hello(self):
        p = qp.device_profile({'device': 'X', 'eye_w': 800, 'eye_h': 600,
                               'refresh_rates': [90]})
        self.assertEqual(p['device'], 'generic')
        self.assertEqual((p['eye_w'], p['eye_h']), (800, 600))

    def test_bad_eye_rejected(self):
        for hello in ({'eye_w': 0, 'eye_h': 100},
                      {'eye_w': 5000, 'eye_h': 100},
                      {'eye_w': 'wide', 'eye_h': 100}):
            with self.assertRaises(ValueError):
                qp.device_profile(hello)


class AimTest(unittest.TestCase):
    def test_forward_is_center(self):
        ident = (0, 0, 0, 1)
        self.assertEqual(qp.aim_to_cursor(ident, ident), (960, 540))

    def test_yaw_right_moves_right(self):
        # R_y(-0.2) takes head-forward to head-right in Y-up right-handed
        # space, so the aim quaternion (0,-sin0.1,0,cos0.1) points right.
        s, c = math.sin(0.1), math.cos(0.1)
        x0, _ = qp.aim_to_cursor((0, 0, 0, 1), (0, 0, 0, 1))
        x1, _ = qp.aim_to_cursor((0, -s, 0, c), (0, 0, 0, 1))
        self.assertGreater(x1, x0)

    def test_pitch_up_moves_up(self):
        _, y0 = qp.aim_to_cursor((0, 0, 0, 1), (0, 0, 0, 1))
        s, c = math.sin(0.1), math.cos(0.1)
        _, y1 = qp.aim_to_cursor((s, 0, 0, c), (0, 0, 0, 1))
        self.assertLess(y1, y0)

    def test_head_relative(self):
        # Aim fixed in world; turning the head left should move the cursor
        # right, same as if the controller had turned right.
        s, c = math.sin(0.1), math.cos(0.1)
        a = qp.aim_to_cursor((0, 0, 0, 1), (0, s, 0, c))
        b = qp.aim_to_cursor((0, -s, 0, c), (0, 0, 0, 1))
        self.assertEqual(a[0] > 960, b[0] > 960)

    def test_clamped(self):
        x, y = qp.aim_to_cursor((0, 1, 0, 0), (0, 0, 0, 1))
        self.assertTrue(0 <= x < 1920 and 0 <= y < 1080)


if __name__ == '__main__':
    unittest.main()
