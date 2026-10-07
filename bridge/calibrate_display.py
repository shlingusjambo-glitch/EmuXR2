#!/usr/bin/env python3
"""Measure each eye's projection on the compositor's display (focal lengths and optical centre, in pixels).

Holds the head still through the guest injector, shows the view turned by known yaw, pitch and roll about each eye
in turn (so that eye's view only rotates), and fits the pinhole model that maps one view onto the other
(p' = K R K^-1 p). The flat mesh (bridge/flat_mesh.py) should give exactly stream.FOV. Stop stream.py first (the
injector serves one host). Usage: calibrate_display.py [lens separation in metres]
"""
import math, os, socket, struct, sys, threading, time

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import stream as st

W, H = st.EYE_W, st.CAP_H


class Head:
    """Streams one head pose to the injector at 72 Hz."""

    def __init__(self):
        st.adb('forward', f'tcp:{st.INJECTOR_PORT}', f'tcp:{st.INJECTOR_PORT}')
        self.sock = socket.create_connection(('127.0.0.1', st.INJECTOR_PORT))
        self.p, self.q = (0.0, 0.0, 0.0), (0.0, 0.0, 0.0, 1.0)
        threading.Thread(target=self.run, daemon=True).start()

    def run(self):
        seq = 700000000
        hands = struct.pack('<3I7f4f', 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0) * 2
        while True:
            seq += 1
            self.sock.sendall(struct.pack('<I7f', seq, *self.p, *self.q) + hands)
            time.sleep(1 / 72)


def axis(ax, deg):
    s = math.sin(math.radians(deg) / 2)
    return tuple(s * a for a in ax) + (math.cos(math.radians(deg) / 2),)


def matrix(q):
    return np.array([st.qp._rot_vec(q, e) for e in ((1, 0, 0), (0, 1, 0), (0, 0, 1))]).T


def grey(img):
    return (((img >> 16) & 255) * .3 + ((img >> 8) & 255) * .59 + (img & 255) * .11).astype(np.float32)


def intrinsics(eye):
    """(fx, fy, cx, cy) the stream assumes for an eye: its field of view spread over its half of the display."""
    l, r, u, d = map(math.tan, st.FOV[eye])
    fx, fy = W / (r - l), H / (u - d)
    return fx, fy, -l * fx, u * fy


def warp_error(a, b, R, fx, fy, cx, cy, step=4):
    """Mean |a(K R K^-1 p) - b(p)| over b's pixels that land inside a (the stamp rows excluded)."""
    v, u = np.mgrid[0:H - 16:step, 0:W:step]
    d = np.stack([(u - cx) / fx, -(v - cy) / fy, -np.ones(u.shape)], -1) @ R.T   # b's rays in a's camera
    ok = d[..., 2] < 0
    u0 = cx + fx * d[..., 0] / -d[..., 2]
    v0 = cy - fy * d[..., 1] / -d[..., 2]
    ok &= (u0 >= 0) & (u0 < W - 1) & (v0 >= 0) & (v0 < H - 17)
    if ok.mean() < .3:
        return 1e9
    return float(np.abs(a[v0[ok].astype(int), u0[ok].astype(int)] - b[v[ok], u[ok]]).mean())


def fit(pairs, p):
    """Coordinate descent on (fx, fy, cx, cy) over all (reference, rotated, R) pairs."""
    p = list(p)
    best = sum(warp_error(a, b, R, *p) for a, b, R in pairs)
    for step in (16, 8, 4, 2, 1, .5):
        improved = True
        while improved:
            improved = False
            for i in range(4):
                for s in (step, -step):
                    q = p[:]
                    q[i] += s
                    e = sum(warp_error(a, b, R, *q) for a, b, R in pairs)
                    if e < best:
                        best, p, improved = e, q, True
    return p, best / len(pairs)


def main():
    ipd = float(sys.argv[1]) if len(sys.argv) > 1 else 0.06485
    head, display = Head(), st.Display()
    for eye in (0, 1):
        e = np.array([(-ipd / 2, ipd / 2)[eye], 0, 0])
        shots = {}
        for name, q in (('ref', axis((0, 1, 0), 0)), ('yaw', axis((0, 1, 0), 8)), ('pitch', axis((1, 0, 0), 8)),
                        ('roll', axis((0, 0, 1), 10)), ('ref2', axis((0, 1, 0), 0))):
            head.p, head.q = tuple(e - matrix(q) @ e), q   # turned about this eye
            time.sleep(2)
            shots[name] = (grey(display.current()[:, eye * W:(eye + 1) * W]), q)
        a = shots['ref'][0]
        want = intrinsics(eye)
        got, err = fit([(a, shots[n][0], matrix(shots[n][1])) for n in ('yaw', 'pitch', 'roll')], want)
        noise = warp_error(a, shots['ref2'][0], np.eye(3), *got)
        print(f'eye {eye}: fx {got[0]:.1f} fy {got[1]:.1f} centre {got[2]:.1f} {got[3]:.1f} px; error {err:.2f} '
              f'(unmoved {noise:.2f}); stream assumes {" ".join(f"{v:.1f}" for v in want)}')
    head.p, head.q = (0.0, 0.0, 0.0), (0.0, 0.0, 0.0, 1.0)
    time.sleep(.5)


if __name__ == '__main__':
    main()
