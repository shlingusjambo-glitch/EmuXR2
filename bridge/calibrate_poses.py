#!/usr/bin/env python3
"""Measure how Horizon turns injected IMU poses into OpenXR poses, so the streamer can inject the inverse.

TrackingDataInjection takes the headset's and controllers' IMU poses ("WorldFromImu"); OpenXR apps see the eye
centre (head) and the grip pose, each a fixed transform away. This runs the VR4Mac client inside the guest with
pose logging on, injects known IMU poses straight into input/Injector.java, and solves:
    app = S * imu * T     (S: the app's reference-space offset, a translation; T: the fixed IMU -> app transform)
Stop stream.py first (the injector serves one host). Usage: calibrate_poses.py <client.apk>
Prints T for the head and each hand as position + quaternion (x y z w).
"""
import math, re, socket, struct, subprocess, sys, time

ADB = ['adb', '-s', 'emulator-5554']
CLIENT = 'com.vr4mac.client'


def qmul(a, b):
    ax, ay, az, aw = a; bx, by, bz, bw = b
    return (aw*bx + ax*bw + ay*bz - az*by, aw*by - ax*bz + ay*bw + az*bx,
            aw*bz + ax*by - ay*bx + az*bw, aw*bw - ax*bx - ay*by - az*bz)


def qconj(q): return (-q[0], -q[1], -q[2], q[3])


def rot(q, v): return qmul(qmul(q, (*v, 0.0)), qconj(q))[:3]


def axis(ax, deg):
    s = math.sin(math.radians(deg) / 2)
    return tuple(s * a for a in ax) + (math.cos(math.radians(deg) / 2),)


def sub(a, b): return tuple(x - y for x, y in zip(a, b))


def adb(*a, **k): return subprocess.run(ADB + list(a), capture_output=True, text=True, timeout=60, **k)


def inject(sock, head, hands, seconds=2.5):
    """head/hands: (position, quaternion) IMU poses in the guest's raw space."""
    hand = b''.join(struct.pack('<3I7f4f', 1, 0, 0, *p, *q, 0, 0, 0, 0) for p, q in hands)
    t0, seq = time.time(), 600000000
    while time.time() - t0 < seconds:
        seq += 1
        sock.sendall(struct.pack('<I7f', seq, *head[0], *head[1]) + hand)
        time.sleep(1 / 72)


def latest():
    out = adb('logcat', '-d', '-s', 'VR4Mac:I').stdout
    poses = {}
    for m in re.finditer(r'pose (head|hand (\d))(?: flags \S+)? (?:grip )?([-\d. ]+?)(?: aim|$)', out, re.M):
        key = 'head' if m.group(1) == 'head' else int(m.group(2))
        v = [float(x) for x in m.group(3).split()]
        poses[key] = (tuple(v[:3]), tuple(v[3:7]))
    return poses


def measure(sock, head, hands):
    adb('logcat', '-c')
    inject(sock, head, hands)
    return latest()


def main():
    if len(sys.argv) > 1:
        print(adb('install', '-r', sys.argv[1]).stdout.strip())
    adb('shell', 'setprop debug.vr4mac.poses 1')
    adb('shell', 'monkey -p %s -c android.intent.category.LAUNCHER 1' % CLIENT)
    time.sleep(8)
    adb('forward', 'tcp:7791', 'tcp:7791')
    sock = socket.create_connection(('127.0.0.1', 7791))
    ident = (0.0, 0.0, 0.0, 1.0)
    hands = [((-0.2, -0.4, -0.4), ident), ((0.2, -0.4, -0.4), ident)]
    base = measure(sock, ((0, 0, 0), ident), hands)
    yaw = measure(sock, ((0, 0, 0), axis((0, 1, 0), 90)), hands)
    pitch = measure(sock, ((0, 0, 0), axis((1, 0, 0), 90)), hands)
    turned = measure(sock, ((0, 0, 0), ident), [(p, axis((0, 1, 0), 90)) for p, _ in hands])
    tilted = measure(sock, ((0, 0, 0), ident), [(p, axis((1, 0, 0), 90)) for p, _ in hands])
    if 'head' not in base:
        sys.exit('no pose log from the client: is it focused?')
    import numpy as np

    def mat(q):
        return np.array([rot(q, e) for e in ((1, 0, 0), (0, 1, 0), (0, 0, 1))]).T

    # app.p = S + imu.p + R(imu.q) t: linear in S and t, one block of three rows per measurement
    def solve(samples):
        A = np.vstack([np.hstack([np.eye(3), mat(q)]) for _, q, _ in samples])
        b = np.hstack([np.subtract(app, imu) for imu, _, app in samples])
        x, res, *_ = np.linalg.lstsq(A, b, rcond=None)
        fit = np.abs(A @ x - b).max()
        return x[:3], x[3:], fit

    runs = [(base, ident, hands), (yaw, axis((0, 1, 0), 90), hands), (pitch, axis((1, 0, 0), 90), hands)]
    S, t, fit = solve([((0, 0, 0), q, r['head'][0]) for r, q, _ in runs if 'head' in r])
    print('reference-space offset S', np.round(S, 4), ' head IMU -> eye centre', np.round(t, 4),
          'rotation', [round(v, 4) for v in base['head'][1]], 'fit %.4f m' % fit)
    for h in (0, 1):
        samples = [(hands[h][0], ident, r[h][0]) for r in (base, yaw, pitch) if h in r]
        for r, q in ((turned, axis((0, 1, 0), 90)), (tilted, axis((1, 0, 0), 90))):
            if h in r:
                samples.append((hands[h][0], q, r[h][0]))
        if len(samples) < 2:
            print('hand', h, 'not reported'); continue
        Sh, T, fit = solve(samples)
        print('hand', h, 'IMU -> grip', np.round(T, 4), 'rotation', [round(v, 4) for v in base[h][1]],
              'S', np.round(Sh, 4), 'fit %.4f m' % fit)


if __name__ == '__main__':
    main()
