"""Owned display snapshots, using macOS's tiled pixel rotation when available."""
import ctypes
import sys

import numpy as np


class Buffer(ctypes.Structure):
    _fields_ = [('data', ctypes.c_void_p), ('height', ctypes.c_ulong),
                ('width', ctypes.c_ulong), ('row_bytes', ctypes.c_size_t)]


_rotate = None
if sys.platform == 'darwin':
    try:
        _accelerate = ctypes.CDLL('/System/Library/Frameworks/Accelerate.framework/Accelerate')
        _rotate = _accelerate.vImageRotate90_ARGB8888
        _rotate.argtypes = [ctypes.POINTER(Buffer), ctypes.POINTER(Buffer), ctypes.c_uint8,
                           ctypes.POINTER(ctypes.c_uint8), ctypes.c_uint32]
        _rotate.restype = ctypes.c_long
    except (OSError, AttributeError):
        pass


def snapshot(pixels):
    """Copy uint32 BGRA pixels upright; never retain a view of the changing shared display."""
    height, width = pixels.shape
    if width > height:
        return pixels.copy(order='C')
    if _rotate is not None and pixels.dtype == np.uint32 and pixels.flags.c_contiguous:
        result = np.empty((width, height), dtype=np.uint32)
        source = Buffer(pixels.ctypes.data, height, width, pixels.strides[0])
        target = Buffer(result.ctypes.data, width, height, result.strides[0])
        # kRotate90DegreesClockwise = 3. ARGB8888 rotates all four bytes without color conversion.
        error = _rotate(ctypes.byref(source), ctypes.byref(target), 3, (ctypes.c_uint8 * 4)(), 0)
        if error == 0:
            return result
    return np.array(np.rot90(pixels, -1), copy=True, order='C')
