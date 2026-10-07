#!/usr/bin/env python3
"""Game frame rate and the display's captured frame rate together, with a moving synthetic head (no encoding).

Stop stream.py first. Usage: bench_stream.py [seconds] [package [activity]]"""
import math, re, statistics, subprocess, sys, time

import calibrate_display as cc, stream as st


def main():
    secs = int(sys.argv[1]) if len(sys.argv) > 1 else 30
    pkg = sys.argv[2] if len(sys.argv) > 2 else 'com.SnowStudios.ApeSprint'
    act = sys.argv[3] if len(sys.argv) > 3 else 'com.unity3d.player.UnityPlayerActivity'
    st.start_guest()
    head, d = cc.Head(), st.Display()
    st.adb('shell', f'am force-stop {pkg}; am start -n {pkg}/{act}')
    time.sleep(45)
    st.adb('logcat', '-c')
    last, stamps, t0 = None, set(), time.time()
    while time.time() - t0 < secs:
        head.q = cc.axis((0, 1, 0), 15 * math.sin((time.time() - t0) * 2))
        last, img = d.grab(last)
        if img is None:
            time.sleep(0.001)
        else:
            stamps.add(st.pose_number(img))
    log = subprocess.run([st.ADB, '-s', st.EMU, 'logcat', '-d', '-s', 'VrApi'], capture_output=True).stdout.decode(errors='replace')
    fps = [int(m) for m in re.findall(r'FPS=(\d+)/', log)]
    print(f'{pkg}: game FPS median {statistics.median(fps) if fps else 0}, display {len(stamps) / secs:.1f} new frames/s')


if __name__ == '__main__':
    main()
