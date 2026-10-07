#!/usr/bin/env python3
"""Check that frames captured while the head turns show the world where their pose number says.

Turns the injected head sinusoidally in yaw, records the stamped capture frames, re-aims each one to the reference view
(yaw 0) with the pose it is stamped with, and measures the horizontal misalignment of what remains, separately for the
panels (UI) and the environment. A frame rendered with another pose than its stamp, or UI drawn with a different pose
than the world, shows up as a non-zero offset. Stop stream.py first. Usage: motion_check.py [seconds] [peak deg/s]
"""
import math, os, socket, struct, sys, threading, time

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import stream as st
import calibrate_display as cc


def main():
    seconds = float(sys.argv[1]) if len(sys.argv) > 1 else 6
    peak = float(sys.argv[2]) if len(sys.argv) > 2 else 90
    amp = 25.0
    hz = peak / (2 * math.pi * amp)
    st.adb('forward', f'tcp:{st.INJECTOR_PORT}', f'tcp:{st.INJECTOR_PORT}')
    sock = socket.create_connection(('127.0.0.1', st.INJECTOR_PORT))
    hands = struct.pack('<3I7f4f', 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0) * 2
    yaws, state = {}, {'yaw': 0.0, 'moving': False, 'stop': False}

    def drive():
        seq, t0 = 800000000, None
        while not state['stop']:
            if state['moving']:
                t0 = t0 or time.monotonic()
                state['yaw'] = amp * math.sin(2 * math.pi * hz * (time.monotonic() - t0))
            seq += 1
            yaws[seq] = state['yaw']
            sock.sendall(struct.pack('<I7f', seq, 0, 0, 0, *cc.axis((0, 1, 0), state['yaw'])) + hands)
            time.sleep(1 / 72)
    threading.Thread(target=drive, daemon=True).start()
    display = st.Display()
    time.sleep(2)
    ref = display.current()
    state['moving'] = True
    frames, last, end = [], None, time.monotonic() + seconds
    while time.monotonic() < end:
        last, img = display.grab(last)
        if img is not None:
            frames.append((time.monotonic(), img))
        time.sleep(.002)
    state['stop'] = True
    W, H = st.EYE_W, st.CAP_H
    f, fy, cx, cy = cc.intrinsics(0)
    a = cc.grey(ref[:, :W])
    ui = (a < 90)   # Horizon's panels are dark; the environment is bright
    v, u = np.mgrid[0:H - 16:4, 0:W:4]
    rays = np.stack([(u - cx) / f, -(v - cy) / fy, -np.ones(u.shape)], -1)
    rows = []
    for when, img in frames:
        seq = st.pose_number(img)
        if seq is None or seq not in yaws:
            continue
        yaw = yaws[seq]
        R = cc.matrix(cc.axis((0, 1, 0), yaw)).T   # reference rays into the frame's camera
        d = rays @ R.T
        u1 = cx + f * d[..., 0] / -d[..., 2]
        v1 = cy - fy * d[..., 1] / -d[..., 2]
        b = cc.grey(img[:, :W])
        out = {}
        for name, mask in (('ui', ui[v, u]), ('world', ~ui[v, u])):
            best = None
            for dx in range(-80, 81, 4):   # residual shift, in frame pixels
                uu = (u1 + dx).astype(int); vv = v1.astype(int)
                ok = mask & (uu >= 0) & (uu < W) & (vv >= 0) & (vv < H)
                if ok.sum() < 200:
                    continue
                e = np.abs(b[vv[ok], uu[ok]] - a[v[ok], u[ok]]).mean()
                if best is None or e < best[0]:
                    best = (e, dx)
            out[name] = best
        if out['ui'] and out['world']:
            rows.append((yaw, out['ui'][1], out['world'][1], (yaw - yaws.get(seq - 1, yaw)) * 72))
    if not rows:
        sys.exit('no stamped frames: is the capture running?')
    deg = lambda px: math.degrees(math.atan(px / f))
    print(f'{len(rows)} stamped frames of {len(frames)}; turning up to {peak:.0f} deg/s')
    for yaw, dui, dworld, vel in rows[::max(1, len(rows) // 15)]:
        print(f'  yaw {yaw:6.1f} at {vel:6.1f} deg/s: UI off by {deg(dui):5.1f} deg, world off by {deg(dworld):5.1f} deg')
    vel = np.array([r[3] for r in rows]); ui_off = np.array([deg(r[1]) for r in rows])
    if vel.std() > 1:   # a constant delay shows as offset = -velocity * delay
        print(f'UI offset vs turn speed: {np.polyfit(vel, ui_off, 1)[0] * -1000:.1f} ms of pose lag')
    ui_err = [abs(deg(r[1])) for r in rows]; world_err = [abs(deg(r[2])) for r in rows]
    big = [(deg(r[1]), r[3]) for r in rows if abs(deg(r[1])) > 2]
    print('outliers (offset deg, speed deg/s, implied ms):', [(round(o, 1), round(v), round(-o / v * 1000) if v else None) for o, v in big])
    print(f'UI frames off by more than 1 deg: {np.mean(np.array(ui_err) > 1) * 100:.0f}%, by more than 2: '
          f'{np.mean(np.array(ui_err) > 2) * 100:.0f}%')
    print(f'mean |offset|: UI {np.mean(ui_err):.2f} deg, world {np.mean(world_err):.2f} deg; '
          f'max UI {max(ui_err):.1f}, world {max(world_err):.1f}')


if __name__ == '__main__':
    main()
