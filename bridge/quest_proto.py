"""Pure protocol helpers for the EmuXR2 <-> Quest bridge (no I/O, no deps).

Kept dependency-free so bridge/test_quest_proto.py can exercise the wire
contract and the input mapping on any interpreter.
"""
import math
import struct

TRACKING_LEN = 284

# VR4Hand.flags / buttons bits (common/vr4mac.h).
HAND_ACTIVE = 1
BTN_A = 1
BTN_B = 2
BTN_X = 4
BTN_Y = 8
BTN_MENU = 16
BTN_STICK_CLICK = 32

# Guest landscape display driven through vinput relative moves.
SCREEN_W, SCREEN_H = 1920, 1080

# Aim-ray mapping: half-range (radians) that spans half the screen.
HALF_RANGE_X = 0.5
HALF_RANGE_Y = 0.35


def _quat_mul(a, b):
    ax, ay, az, aw = a
    bx, by, bz, bw = b
    return (aw * bx + ax * bw + ay * bz - az * by,
            aw * by - ax * bz + ay * bw + az * bx,
            aw * bz + ax * by - ay * bx + az * bw,
            aw * bw - ax * bx - ay * by - az * bz)


def _quat_conj(q):
    x, y, z, w = q
    return (-x, -y, -z, w)


def _rot_vec(q, v):
    x, y, z = _quat_mul(_quat_mul(q, (v[0], v[1], v[2], 0.0)), _quat_conj(q))[:3]
    return (x, y, z)


def parse_tracking(payload):
    """Parse a VR4_TRACKING payload; raises ValueError on short input."""
    if len(payload) < TRACKING_LEN:
        raise ValueError(f'tracking payload {len(payload)} < {TRACKING_LEN}')

    def hand_at(off):
        flags, buttons = struct.unpack('<2I', payload[off:off + 8])
        aim = struct.unpack('<7f', payload[off + 8:off + 36])
        grip = struct.unpack('<7f', payload[off + 36:off + 64])
        trigger, squeeze, sx, sy = struct.unpack('<4f', payload[off + 64:off + 80])
        return {'flags': flags, 'buttons': buttons, 'aim': aim,
                'grip': grip, 'trigger': trigger, 'squeeze': squeeze,
                'stick': (sx, sy)}

    return {
        'time_ns': struct.unpack('<Q', payload[:8])[0],
        'head': struct.unpack('<7f', payload[8:36]),
        'hands': (hand_at(124), hand_at(204)),
    }


def device_profile(hello):
    """Choose encode parameters from a HELLO dict.

    Quest 1 is fingerprinted by its native eye size; it gets the guest's
    native per-eye pixels (no upscale blur, 44% fewer pixels to encode)
    and a bitrate its AVC decoder sustains. Unknown devices echo HELLO.
    """
    try:
        eye_w = int(hello.get('eye_w', 0))
        eye_h = int(hello.get('eye_h', 0))
    except (TypeError, ValueError):
        raise ValueError('bad eye size in HELLO')
    if not (0 < eye_w <= 2048 and 0 < eye_h <= 2048):
        raise ValueError(f'eye size out of range: {eye_w}x{eye_h}')
    rates = hello.get('refresh_rates') or [72]
    fps = min(max(int(r) for r in rates), 72)
    if fps < 30:
        fps = 30
    if (eye_w, eye_h) == (1216, 1344):
        # Quest 1: encode at its native eye size. (Tried the guest's
        # 960x1080 pre-warped pixels instead: the QCOM AVC decoder
        # dropped 5-7 frames/s on the non-16-aligned 1080 height, while
        # native size holds a clean 30/0.)
        return {'device': 'quest1', 'codec': 'h264', 'eye_w': eye_w,
                'eye_h': eye_h, 'fps': fps, 'bitrate': 20_000_000}
    return {'device': 'generic', 'codec': 'h264', 'eye_w': eye_w,
            'eye_h': eye_h, 'fps': fps, 'bitrate': 20_000_000}


def aim_to_cursor(aim_q, head_q, screen=(SCREEN_W, SCREEN_H)):
    """Map a controller aim pose to guest screen pixels.

    Treats the aim ray as a laser on a plane fixed in head space: yaw/pitch
    of the aim direction relative to head forward map linearly to x/y.
    Returns integer (x, y) clamped to the screen.
    """
    rel = _quat_mul(_quat_conj(head_q), aim_q)
    fx, fy, fz = _rot_vec(rel, (0.0, 0.0, -1.0))
    yaw = math.atan2(fx, -fz) if -fz > 1e-6 else math.copysign(math.pi / 2, fx)
    pitch = math.asin(max(-1.0, min(1.0, fy)))
    w, h = screen
    x = w / 2 + yaw / HALF_RANGE_X * (w / 2)
    y = h / 2 - pitch / HALF_RANGE_Y * (h / 2)
    return (max(0, min(w - 1, int(round(x)))),
            max(0, min(h - 1, int(round(y)))))
