#!/usr/bin/env python3
"""Press the guest menu (Home) button inside a running game and report whether the universal menu opened over it.

Synthetic head and controllers (stop stream.py first: the injector serves one host). Saves screenshots/menu-<n>.png
and prints the foreground activity and the overlay process's errors. Usage: menu_check.py [package [activity]]"""
import socket, struct, subprocess, sys, threading, time

import numpy as np

import stream as st

HOME = 2


class Rig:
    """Head plus two posed controllers at 72 Hz; pressed[h] holds hand h's button bits."""

    def __init__(self):
        st.adb('forward', f'tcp:{st.INJECTOR_PORT}', f'tcp:{st.INJECTOR_PORT}')
        self.sock = socket.create_connection(('127.0.0.1', st.INJECTOR_PORT))
        self.pressed = [0, 0]
        threading.Thread(target=self.run, daemon=True).start()

    def run(self):
        seq = 710000000
        while True:
            seq += 1
            out = struct.pack('<I7f', seq, 0, 0, 0, 0, 0, 0, 1)
            for h, x in enumerate((-0.2, 0.2)):
                out += struct.pack('<3I7f4f', 1, self.pressed[h], self.pressed[h], x, -0.3, -0.35, 0, 0, 0, 1, 0, 0, 0, 0)
            self.sock.sendall(out)
            time.sleep(1 / 72)


def save(img, name):
    rgb = np.stack([(img >> 16) & 255, (img >> 8) & 255, img & 255], -1).astype(np.uint8)[::2, ::2]
    open('/tmp/menu.ppm', 'wb').write(b'P6 %d %d 255\n' % (rgb.shape[1], rgb.shape[0]) + rgb.tobytes())
    subprocess.run(['sips', '-s', 'format', 'png', '/tmp/menu.ppm', '--out', f'../screenshots/{name}.png'],
                   capture_output=True)


def top():
    out = st.adb('shell', 'dumpsys activity activities | grep -m1 topResumedActivity').stdout.strip()
    return out.split()[-2] if out else '?'


def main():
    pkg = sys.argv[1] if len(sys.argv) > 1 else 'com.SnowStudios.ApeSprint'
    act = sys.argv[2] if len(sys.argv) > 2 else 'com.unity3d.player.UnityPlayerActivity'
    st.start_guest()
    rig, d = Rig(), st.Display()
    st.adb('shell', f'am force-stop {pkg}; am start -n {pkg}/{act}')
    time.sleep(40)
    save(d.current(), 'menu-0')
    print('before:', top())
    st.adb('logcat', '-c')
    rig.pressed[0] = HOME
    time.sleep(0.2)
    rig.pressed[0] = 0
    time.sleep(8)
    save(d.current(), 'menu-1')
    print('after:', top())
    log = subprocess.run([st.ADB, '-s', st.EMU, 'logcat', '-d'], capture_output=True).stdout.decode(errors='replace').splitlines()
    for l in [l for l in log if 'Overlay' in l or 'XR_ERROR' in l or ' F ' in l][-25:]:
        print(l[:220])


if __name__ == '__main__':
    main()
