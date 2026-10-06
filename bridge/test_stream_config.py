"""Self-check for stream.negotiate (run with the streamer's python)."""
import stream as st

Q1 = {'device': 'Quest', 'eye_w': 1216, 'eye_h': 1344,
      'refresh_rates': [72], 'codecs': ['hevc', 'h264']}

prof, config = st.negotiate(Q1)
assert (prof['eye_w'], prof['eye_h']) == (960, 1088), prof
assert (prof['fps'], prof['bitrate']) == (72, 20_000_000), prof
# the encoded frame is exactly what CONFIG advertises (the decoder surface)
assert (config['eye_w'], config['eye_h']) == (960, 1088), config
assert config['eye_w'] * 2 == prof['eye_w'] * 2, config
assert config['fps'] == 72 and config['codec'] == 'h264', config
assert len(config['fov']) == 2, config

# unknown headsets echo their HELLO size, capped at 72 fps
prof2, config2 = st.negotiate({'device': 'X', 'eye_w': 800,
                               'eye_h': 600, 'refresh_rates': [90]})
assert (prof2['eye_w'], prof2['eye_h'], prof2['fps']) == (960, 1088, 72), prof2
assert (config2['eye_w'], config2['eye_h']) == (960, 1088), config2

# bad HELLO is rejected, never encoded
for bad in ({'eye_w': 0, 'eye_h': 100},
            {'eye_w': 5000, 'eye_h': 100},
            {'eye_w': 'wide', 'eye_h': 100}):
    try:
        st.negotiate(bad)
    except ValueError:
        pass
    else:
        raise SystemExit(f'should have raised: {bad}')


# the injector readiness wait: true once the guest reports listening, false on timeout
class _R:
    def __init__(self, rc):
        self.returncode = rc


_real_adb = st.adb
st.adb = lambda *a, **k: _R(0)
assert st.wait_injector(timeout=5) is True
calls = []
st.adb = lambda *a, **k: (calls.append(a), _R(1))[1]
assert st.wait_injector(timeout=0) is False
st.adb = _real_adb


# configure + emit round-trip over a socketpair: the encoder size always matches
# what CONFIG advertises, and VIDEO headers carry (frame_id, tracking time)
import json
import socket as _socket
import struct as _struct

import numpy as _np

a, b = _socket.socketpair()
# the test reads synchronously (no peer thread): size the buffers well above the
# ~11 KB this round-trip sends, or the 10 KB VIDEO packet deadlocks the sender
for s in (a, b):
    s.setsockopt(_socket.SOL_SOCKET, _socket.SO_SNDBUF, 8 << 20)
    s.setsockopt(_socket.SOL_SOCKET, _socket.SO_RCVBUF, 8 << 20)
sess = st.Session(a, None, None)
sess.configure(dict(Q1))


def _recv_pkt(s):
    hdr = b''
    while len(hdr) < 5:
        chunk = s.recv(5 - len(hdr))
        assert chunk, 'peer closed'
        hdr += chunk
    ptype, plen = hdr[0], _struct.unpack('<I', hdr[1:])[0]
    payload = b''
    while len(payload) < plen:
        chunk = s.recv(plen - len(payload))
        assert chunk, 'peer closed'
        payload += chunk
    return ptype, payload


ptype, payload = _recv_pkt(b)
assert ptype == 2, ptype
config = json.loads(payload)
assert (config['eye_w'], config['eye_h'], config['fps']) == (960, 1088, 72), config
assert (sess.encoder.width, sess.encoder.height) == (1920, 1088)
px = _np.zeros((st.ENC_H, st.CAP_W), _np.uint32)
sess.emit(px, 123456)
ptype, payload = _recv_pkt(b)
assert ptype == 4, ptype
frame_id, when, flags = _struct.unpack('<QQB', payload[:17])
assert (frame_id, when) == (0, 123456), (frame_id, when)
assert len(payload) > 17, 'no h264 bytes'
sess.emit(px, 123457)   # frozen-display resend reuses the pixels, moves frame_id
ptype, payload = _recv_pkt(b)
assert ptype == 4, ptype
frame_id, when, flags = _struct.unpack('<QQB', payload[:17])
assert (frame_id, when) == (1, 123457), (frame_id, when)
a.close()
b.close()


# frozen display: run() with a counter that never advances still emits a frame
# a second, so the client never hits its 5 s read timeout and re-handshakes
import threading as _threading
import time as _time


class _FrozenDisplay:
    def __init__(self, img):
        self.img = img

    def grab(self, last):
        return last, None

    def current(self):
        return self.img


c, d = _socket.socketpair()
for s in (c, d):
    s.setsockopt(_socket.SOL_SOCKET, _socket.SO_SNDBUF, 8 << 20)
    s.setsockopt(_socket.SOL_SOCKET, _socket.SO_RCVBUF, 8 << 20)
d.settimeout(10)
frozen = st.Session(c, _FrozenDisplay(_np.zeros((st.CAP_H, st.CAP_W), _np.uint32)), None)
frozen.configure(dict(Q1))
assert _recv_pkt(d)[0] == 2
t = _threading.Thread(target=frozen.run, daemon=True)
t.start()
_time.sleep(3.5)
frozen.stop = True
t.join(timeout=10)
assert not t.is_alive(), 'run loop did not stop'
c.close()   # unblock the leaked reader thread
videos = []
d.settimeout(5)
try:
    while True:
        ptype, payload = _recv_pkt(d)
        if ptype == 4:
            videos.append(_struct.unpack('<QQB', payload[:17]))
except Exception:
    pass
assert len(videos) >= 2, videos
assert [v[0] for v in videos] == list(range(len(videos))), videos
assert all(v[1] == 0 for v in videos), videos   # no tracking yet: decoded, not shown
print('ok')


# A stalled frame must retain the exact original pose, even when newer tracking
# exists. Changing this timestamp would reproject old pixels from the wrong pose.
class _OnceDisplay(_FrozenDisplay):
    def grab(self, last):
        return (1, self.img.copy()) if last is None else (last, None)

img = _np.zeros((st.CAP_H, st.CAP_W), _np.uint32)
bits = 0xD | (42 << 4)
for i in range(st.MARK_BLOCKS):
    img[-st.MARK:, i*st.MARK:(i+1)*st.MARK] = 0xffffffff if (bits >> i) & 1 else 0
c, d = _socket.socketpair()
for sock in (c, d):
    sock.setsockopt(_socket.SOL_SOCKET, _socket.SO_SNDBUF, 8 << 20)
    sock.setsockopt(_socket.SOL_SOCKET, _socket.SO_RCVBUF, 8 << 20)
session = st.Session(c, _OnceDisplay(img), None)
session.times = {42: 123000000, 43: 456000000}
session.seq = 43
session.configure(dict(Q1))
assert _recv_pkt(d)[0] == 2
t = _threading.Thread(target=session.run)
t.start()
d.settimeout(5)
for _ in range(3):
    ptype, packet = _recv_pkt(d)
    assert ptype == 4
    assert _struct.unpack('<QQB', packet[:17])[1] == 123000000
session.stop = True
t.join(timeout=5)
assert not t.is_alive()
c.close(); d.close()
print('captured pose preserved on frozen repeats')
