#!/usr/bin/env python3
"""Count magenta pixels (the colour texture decoders write for blocks they can't decode) in a game, looking around.

Launches a package with a synthetic head (stop stream.py first: the injector serves one host), turns through a few
yaw angles and reports the magenta share of each eye view; saves the frames to screenshots/pink-<n>.png.
Usage: pink_check.py [package [activity]]"""
import subprocess, sys, time

import numpy as np

import calibrate_display as cc, stream as st


def magenta(img):
    r, g, b = (img >> 16) & 255, (img >> 8) & 255, img & 255
    return float(((r > 150) & (b > 150) & (g < 90)).mean())


def main():
    pkg = sys.argv[1] if len(sys.argv) > 1 else 'com.SnowStudios.ApeSprint'
    act = sys.argv[2] if len(sys.argv) > 2 else 'com.unity3d.player.UnityPlayerActivity'
    st.start_guest()   # stream.py takes its helpers down when it exits
    head, d = cc.Head(), st.Display()
    st.adb('shell', f'am force-stop {pkg}; am start -n {pkg}/{act}')
    time.sleep(45)
    for n, yaw in enumerate((0, 90, 180, 270)):
        head.q = cc.axis((0, 1, 0), yaw)
        time.sleep(4)
        img = d.current()
        print(f'yaw {yaw}: magenta {magenta(img):.4f}', flush=True)
        rgb = np.stack([(img >> 16) & 255, (img >> 8) & 255, img & 255], -1).astype(np.uint8)[::2, ::2]
        open('/tmp/pink.ppm', 'wb').write(b'P6 %d %d 255\n' % (rgb.shape[1], rgb.shape[0]) + rgb.tobytes())
        subprocess.run(['sips', '-s', 'format', 'png', '/tmp/pink.ppm', '--out', f'../screenshots/pink-{n}.png'],
                       capture_output=True)


if __name__ == '__main__':
    main()
