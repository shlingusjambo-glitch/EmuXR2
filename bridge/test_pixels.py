import unittest
from unittest.mock import patch

import numpy as np
import pixels


class PixelSnapshotTest(unittest.TestCase):
    def test_native_and_fallback_preserve_every_pixel(self):
        rng = np.random.default_rng(42)
        for shape in ((17, 9), (9, 17), (2560, 1600)):
            source = rng.integers(0, 2**32, shape, dtype=np.uint32)
            expected = source if shape[1] > shape[0] else np.rot90(source, -1)
            native = pixels.snapshot(source)
            with patch.object(pixels, '_rotate', None):
                fallback = pixels.snapshot(source)
            for result in (native, fallback):
                np.testing.assert_array_equal(result, expected)
                self.assertTrue(result.flags.c_contiguous)
                self.assertFalse(np.shares_memory(result, source))

    def test_strided_input_uses_safe_fallback(self):
        source = np.arange(500, dtype=np.uint32).reshape(25, 20)[:, ::2]
        np.testing.assert_array_equal(pixels.snapshot(source), np.rot90(source, -1))

    def test_rgba_to_bgra_native_and_fallback(self):
        rgba = np.frombuffer(bytes([1, 2, 3, 4, 10, 20, 30, 40] * 6), np.uint32).reshape(3, 4).copy()
        bgra = np.frombuffer(bytes([3, 2, 1, 4, 30, 20, 10, 40] * 6), np.uint32).reshape(3, 4)
        np.testing.assert_array_equal(pixels.rgba_to_bgra(rgba.copy()), bgra)
        with patch.object(pixels, '_permute', None):
            np.testing.assert_array_equal(pixels.rgba_to_bgra(rgba.copy()), bgra)


if __name__ == '__main__':
    unittest.main()
