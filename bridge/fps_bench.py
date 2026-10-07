#!/usr/bin/env python3
"""Median VrApi FPS / prediction of a game with a still synthetic head (stop stream.py first).

Usage: fps_bench.py [seconds] [package [activity]]. Boot the emulator however you're testing first (e.g.
EMUARGS='-feature X' ../boot.sh); this starts the guest helpers, launches the game and reads the runtime's FPS lines."""
import re, statistics, subprocess, sys, time

import calibrate_display as cc, stream as st


def main():
    secs = int(sys.argv[1]) if len(sys.argv) > 1 else 40
    pkg = sys.argv[2] if len(sys.argv) > 2 else 'com.SnowStudios.ApeSprint'
    act = sys.argv[3] if len(sys.argv) > 3 else 'com.unity3d.player.UnityPlayerActivity'
    st.start_guest()
    head = cc.Head()
    st.adb('shell', f'am force-stop {pkg}; am start -n {pkg}/{act}')
    time.sleep(45)
    st.adb('logcat', '-c')
    time.sleep(secs)
    log = subprocess.run([st.ADB, '-s', st.EMU, 'logcat', '-d', '-s', 'VrApi'], capture_output=True).stdout.decode(errors='replace')
    fps = [int(m) for m in re.findall(r'FPS=(\d+)/', log)]
    prd = [int(m) for m in re.findall(r'Prd=(\d+)ms', log)]
    print(f'{pkg}: FPS median {statistics.median(fps) if fps else 0} (n {len(fps)}, min {min(fps, default=0)}, '
          f'max {max(fps, default=0)}), Prd median {statistics.median(prd) if prd else 0} ms')


if __name__ == '__main__':
    main()
