#!/usr/bin/env python3
"""Measure the projection of Horizon's casting capture (per eye: focal length and optical centre, in pixels).

Holds the head still through the guest injector, captures the view at known yaw and pitch, and fits the pinhole
model that maps one view onto the other (p' = K R K^-1 p). Stop stream.py first (the injector serves one host).
Usage: calibrate_capture.py [aspect fov adjustment in degrees]   (restarts the guest VR runtime with that capture)
"""
import math, os, socket, struct, sys, threading, time

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import stream as st

W, H = st.CAP_W // 2, st.CAP_H


class Head:
    """Streams one head orientation to the injector at 72 Hz."""

    def __init__(self):
        st.adb('forward', f'tcp:{st.INJECTOR_PORT}', f'tcp:{st.INJECTOR_PORT}')
        self.sock = socket.create_connection(('127.0.0.1', st.INJECTOR_PORT))
        self.q = (0.0, 0.0, 0.0, 1.0)
        threading.Thread(target=self.run, daemon=True).start()

    def run(self):
        seq = 700000000
        hands = struct.pack('<3I7f4f', 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0) * 2
        while True:
            seq += 1
            self.sock.sendall(struct.pack('<I7f', seq, 0, 0, 0, *self.q) + hands)
            time.sleep(1 / 72)


def axis(ax, deg):
    s = math.sin(math.radians(deg) / 2)
    return tuple(s * a for a in ax) + (math.cos(math.radians(deg) / 2),)


def matrix(q):
    return np.array([st.qp._rot_vec(q, e) for e in ((1, 0, 0), (0, 1, 0), (0, 0, 1))]).T


def grey(img):
    return (((img >> 16) & 255) * .3 + ((img >> 8) & 255) * .59 + (img & 255) * .11).astype(np.float32)


def warp_error(a, b, R, f, cx, cy, step=4):
    """Mean |a(K R K^-1 p) - b(p)| over b's pixels that land inside a."""
    v, u = np.mgrid[0:H - 16:step, 0:W:step]
    d = np.stack([u - cx, -(v - cy), -np.full(u.shape, f)], -1) @ R.T   # b's rays in a's camera
    ok = d[..., 2] < 0
    u0 = cx + f * d[..., 0] / -d[..., 2]
    v0 = cy - f * d[..., 1] / -d[..., 2]
    ok &= (u0 >= 0) & (u0 < W - 1) & (v0 >= 0) & (v0 < H - 17)
    if ok.mean() < .3:
        return 1e9
    return float(np.abs(a[v0[ok].astype(int), u0[ok].astype(int)] - b[v[ok], u[ok]]).mean())


def fit(pairs, f, cx, cy):
    """Coordinate descent on (f, cx, cy) over all (reference, rotated, R) pairs."""
    best = sum(warp_error(a, b, R, f, cx, cy) for a, b, R in pairs)
    for step in (32, 16, 8, 4, 2, 1, .5):
        improved = True
        while improved:
            improved = False
            for df, dx, dy in ((step, 0, 0), (-step, 0, 0), (0, step, 0), (0, -step, 0), (0, 0, step), (0, 0, -step)):
                e = sum(warp_error(a, b, R, f + df, cx + dx, cy + dy) for a, b, R in pairs)
                if e < best:
                    best, f, cx, cy, improved = e, f + df, cx + dx, cy + dy, True
    return f, cx, cy, best


def main():
    adj = float(sys.argv[1]) if len(sys.argv) > 1 else 0.0
    head = Head()
    st.adb('shell', 'for p in $(pgrep -f "[a]pp_process.*Capture"); do kill $p; done; kill $(pidof com.oculus.vrruntimeservice)')
    time.sleep(40)
    st.adb('shell', f'CLASSPATH=/data/local/tmp/capture.dex setsid nohup app_process / Capture {st.CAP_W} {st.CAP_H} '
                    f'{st.CAP_W} {st.CAP_H} {adj} > /data/local/tmp/capture.log 2>&1 < /dev/null &')
    time.sleep(8)
    display = st.Display()
    shots = {}
    for name, q in (('ref', axis((0, 1, 0), 0)), ('yaw', axis((0, 1, 0), 8)), ('ref2', axis((0, 1, 0), 0)),
                    ('pitch', axis((1, 0, 0), 8)), ('roll', axis((0, 0, 1), 10))):
        head.q = q
        time.sleep(2)
        shots[name] = (display.current(), q)
    ref = shots['ref'][0]
    guess = (W / 2) / math.tan(math.radians(80 + adj) / 2)
    for eye in (0, 1):
        a = grey(ref[:, eye * W:(eye + 1) * W])
        pairs = [(a, grey(shots[n][0][:, eye * W:(eye + 1) * W]), matrix(shots[n][1])) for n in ('yaw', 'pitch', 'roll')]
        f, cx, cy, e = fit(pairs, guess, W / 2, H / 2)
        noise = warp_error(a, grey(shots['ref2'][0][:, eye * W:(eye + 1) * W]), np.eye(3), f, cx, cy)
        print(f'eye {eye}: f {f:.1f} px, centre {cx:.1f} {cy:.1f}; error {e / 3:.2f} (unmoved {noise:.2f}); '
              f'model guess f {guess:.1f}')


if __name__ == '__main__':
    main()
