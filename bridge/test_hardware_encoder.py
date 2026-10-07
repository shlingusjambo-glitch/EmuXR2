"""Hardware encoding must return every frame immediately, without pose reordering."""
import os
import socket
import sys
import unittest
from unittest.mock import patch

import av
import numpy as np
import stream


class HardwareEncoderTests(unittest.TestCase):
    def test_unavailable_hardware_falls_back_to_software(self):
        left, right = socket.socketpair()
        create = av.CodecContext.create
        def unavailable(name, mode):
            if name == 'h264_videotoolbox': raise ValueError('injected hardware failure')
            return create(name, mode)
        try:
            session = stream.Session(left, None, None)
            with patch.dict(os.environ, {'EMUXR2_ENCODER': 'auto'}), patch.object(stream.sys, 'platform', 'darwin'):
                with patch.object(stream.av, 'CodecContext') as codecs:
                    codecs.create.side_effect = unavailable
                    session.configure({'device': 'fallback test', 'eye_w': 1280, 'eye_h': 1600, 'refresh_rates': [72],
                                       'codecs': ['h264'], 'features': ['fov']})
            self.assertEqual(session.encoder.name, 'libx264')
        finally:
            left.close()
            right.close()

    @unittest.skipUnless(sys.platform == 'darwin', 'VideoToolbox requires macOS')
    def test_one_frame_in_one_frame_out(self):
        left, right = socket.socketpair()
        try:
            session = stream.Session(left, None, None)
            with patch.dict(os.environ, {'EMUXR2_ENCODER': 'auto'}):
                session.configure({'device': 'encoder test', 'eye_w': 1280, 'eye_h': 1600,
                                   'refresh_rates': [72], 'codecs': ['h264'], 'features': ['fov']})
            if session.encoder.name != 'h264_videotoolbox':
                self.skipTest('hardware encoder unavailable')
            decoder = av.CodecContext.create('h264', 'r')
            image = np.zeros((1600, 2560, 3), np.uint8)
            for index in range(12):
                image[:, :1280, 0] = 40 + index * 10
                image[:, 1280:, 1] = 200 - index * 10
                frame = av.VideoFrame.from_ndarray(image, format='rgb24').reformat(format='nv12')
                frame.pts = index * 13889
                packets = session.encoder.encode(frame)
                self.assertEqual(len(packets), 1, 'hardware encoder buffered a VR frame')
                self.assertEqual(packets[0].pts, frame.pts, 'hardware encoder reordered poses')
                self.assertEqual(len(decoder.decode(packets[0])), 1, 'decoder buffered a VR frame')
        finally:
            left.close()
            right.close()
