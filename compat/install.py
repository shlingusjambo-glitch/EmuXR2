#!/usr/bin/env python3
"""Prepare firmware init files for desktop tracking without changing app code."""
import pathlib, sys
p = pathlib.Path(sys.argv[1])
for name in ('trackingservice-net.rc', 'mrsystemservice.rc'):
    f = p / name
    s = f.read_text()
    marker = '    class late_start\n'
    env = '    setenv LD_PRELOAD /system/lib64/libdeviceid_macvr.so\n    setenv LD_LIBRARY_PATH /system/lib64/macvr:/system/lib64\n'
    if env not in s: s = s.replace(marker, marker + env)
    s = s.replace('0-3,7', '0-3')
    f.write_text(s)
s = 'on early-boot\n'
for name in ('tracking','object_tracking','handtracking','hand_tracking','constellation'):
    s += f'    mkdir /dev/cpuset/{name} 0755 root system\n    write /dev/cpuset/{name}/cpus 0-3\n    write /dev/cpuset/{name}/mems 0\n    chmod 0666 /dev/cpuset/{name}/tasks\n'
(p / 'macvr-cpusets.rc').write_text(s)
