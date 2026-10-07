#!/usr/bin/env python3
"""vtenc reads the pose stamp and encodes an upright picture whichever way the emulator's window is turned.
Builds a stamped upright frame, hands it to vtenc in all four orientations, and decodes what comes out."""
import os, struct, subprocess, tempfile, unittest

import av
import numpy as np

import stream as st

W, H = 2560, 1600


def upright(pose):
    img = np.zeros((H, W, 4), np.uint8)
    img[..., 3] = 255
    img[:200, :200, 0] = 255   # red square top-left: shows the picture's way up after decoding
    bits = 0xD | pose << 4
    for i in range(36):
        if bits >> i & 1:
            img[H - 8:, 8 * i:8 * i + 8, :3] = 255
    return img


class Orientations(unittest.TestCase):
    @unittest.skipUnless(os.path.exists(st.VTENC), 'build vtenc first (build_vtenc.sh)')
    def test_every_window_rotation(self):
        # raw frames that each orientation turns upright: 0 clockwise 90, 1 none, 2 counter-clockwise 90, 3 180
        for o, k in ((0, 1), (1, 0), (2, -1), (3, 2)):
            with self.subTest(orientation=o), tempfile.TemporaryDirectory() as d:
                path = os.path.join(d, 'frame')
                raw = np.ascontiguousarray(np.rot90(upright(1234 + o), k))
                open(path, 'wb').write(raw.tobytes())
                open(path + '.seq', 'wb').write(struct.pack('<Q', 0))
                p = subprocess.Popen([st.VTENC, path, str(W), str(H), '72', '20000000'], stdin=subprocess.PIPE, stdout=subprocess.PIPE)
                stamp, data, n = None, None, 0
                while data is None:
                    kind = p.stdout.read(1)[0]
                    if kind == 2:   # status, once a second: vtenc is running, so a new frame number is seen
                        p.stdout.read(16)
                        n += 1
                        self.assertLess(n, 10, 'no frame encoded')
                        with open(path + '.seq', 'r+b') as f:
                            f.write(struct.pack('<Q', n))
                        continue
                    n, stamp, _ = struct.unpack('<IqB', p.stdout.read(13))
                    data = p.stdout.read(n)
                p.stdin.close(); p.wait(5)
                self.assertEqual(stamp, 1234 + o)
                frames = av.CodecContext.create('h264', 'r').decode(av.Packet(data))
                rgb = frames[0].to_ndarray(format='rgb24')
                self.assertEqual(rgb.shape[:2], (H, W))
                self.assertGreater(rgb[100, 100, 0], 200)      # red stays top-left
                self.assertLess(rgb[100, W - 100, 0], 50)
                self.assertLess(rgb[H - 100, 100, 0], 50)


if __name__ == '__main__':
    unittest.main()
