#!/usr/bin/env python3
"""EmuXR2 -> Quest streaming (the VR4Mac client, com.vr4mac.client, protocol in VR4Mac's common/vr4mac.h).

Frames: Horizon's own casting capture, both eyes undistorted side by side (input/Capture.java), shown on the
emulator's display and read from its shared framebuffer. Each frame carries the number of the head pose it was
rendered with (stamped by the EGL shim), so its VIDEO packet names the headset's exact tracking sample and the
headset reprojects it correctly. Head poses go straight to the guest's injector (input/Injector.java) over
adb forward, one per TRACKING packet.

Usage: stream.py   (emulator running, Quest on USB with the VR4Mac client; needs PyAV with VideoToolbox)
"""
import ctypes
import json
import math
import mmap
import os
import socket
import struct
import subprocess
import sys
import threading
import time
from fractions import Fraction

import av
import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import quest_proto as qp

ADB = os.environ.get('ANDROID_SDK_ROOT', os.path.expanduser('~/Library/Android/sdk')) + '/platform-tools/adb'
EMU = 'emulator-5554'
HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, '..', 'input', 'out')
INJECTOR_PORT, QUEST_PORT = 7791, 9945

# The capture: 1920x1080, both eyes side by side. Measured projection (calibration: known head rotations,
# matched features, p' = K R K^-1 p): square pixels, f = 572.3 px, optical centre x 501.6 / 458.4, y 587.6.
CAP_W, CAP_H, EYE_W = 1920, 1080, 960
ENC_H = 1088   # encoded height: 16-aligned (the Quest 1 AVC decoder drops frames at 1080); extra rows are black
FOCAL, CENTRE_X, CENTRE_Y = 572.3, (501.6, 458.4), 587.6
FOV = [[math.atan(-cx / FOCAL), math.atan((EYE_W - cx) / FOCAL),
        math.atan(CENTRE_Y / FOCAL), math.atan(-(ENC_H - CENTRE_Y) / FOCAL)] for cx in CENTRE_X]
MARK_BLOCKS, MARK = 36, 8   # pose-number stamp: 36 blocks of 8x8 px along the bottom-left edge
FPS, BITRATE = 60, 25_000_000
LOG = open('/tmp/emuxr2-stream.log', 'a', buffering=1)


def log(*a):
    msg = ' '.join(str(x) for x in a)
    print(msg, flush=True)
    LOG.write(time.strftime('%H:%M:%S ') + msg + '\n')


def adb(*args, serial=EMU, timeout=20):
    return subprocess.run([ADB, '-s', serial, *args], capture_output=True, text=True, timeout=timeout)


def quest_serial():
    for line in subprocess.run([ADB, 'devices'], capture_output=True, text=True).stdout.splitlines()[1:]:
        f = line.split()
        if len(f) == 2 and f[1] == 'device' and not f[0].startswith('emulator-'):
            return f[0]
    return None


def start_guest(capture_only=False):
    """(Re)start the injector and the capture in the guest; their dex files come from input/build.sh."""
    for dex in ('injector', 'capture'):
        adb('push', f'{OUT}/{dex}.dex', f'/data/local/tmp/{dex}.dex')
    which = 'Capture' if capture_only else '(Injector|Capture)'
    adb('shell', f'for p in $(pgrep -f "[a]pp_process.*{which}"); do kill $p; done')
    time.sleep(0.5)
    if not capture_only:
        adb('shell', 'CLASSPATH=/data/local/tmp/injector.dex setsid nohup app_process /system/bin Injector '
                     '> /data/local/tmp/injector.log 2>&1 < /dev/null &')
    adb('shell', f'CLASSPATH=/data/local/tmp/capture.dex setsid nohup app_process / Capture {CAP_W} {CAP_H} '
                 f'{CAP_W} {CAP_H} > /data/local/tmp/capture.log 2>&1 < /dev/null &')
    adb('forward', f'tcp:{INJECTOR_PORT}', f'tcp:{INJECTOR_PORT}')


class Display:
    """The emulator's shared framebuffer: header (width, height, fps, frame counter, ...) then BGRA pixels."""

    def __init__(self):
        fd = ctypes.CDLL(None).shm_open(b'videmulator5554', os.O_RDONLY, 0)
        if fd < 0:
            sys.exit('emulator framebuffer not found: is the emulator running?')
        self.m = mmap.mmap(fd, os.fstat(fd).st_size, access=mmap.ACCESS_READ)
        os.close(fd)
        self.w, self.h = struct.unpack('<2I', self.m[:8])
        self.px = np.frombuffer(self.m, np.uint32, self.w * self.h, 24).reshape(self.h, self.w)

    def counter(self):
        return struct.unpack('<I', self.m[12:16])[0]

    def grab(self, last):
        """The next whole frame after `last` as an upright side-by-side image, or (last, None)."""
        n = self.counter()
        if n == last:
            return last, None
        raw = self.px.copy()
        if self.counter() != n:   # written while copying: take the next one
            return last, None
        up = raw if self.w > self.h else np.rot90(raw, -1)   # the panel scans out portrait
        return n, np.ascontiguousarray(up)


def pose_number(img):
    """The stamped pose number, or None when this isn't a capture frame."""
    row = img[CAP_H - MARK // 2, MARK // 2:MARK_BLOCKS * MARK:MARK]
    bits = ((row >> 8) & 0xFF) > 127   # green channel
    if list(bits[:4]) != [True, False, True, True]:
        return None
    return sum(int(b) << i for i, b in enumerate(bits[4:]))


class Guest:
    """The pose injector's socket, reconnected (with the guest helpers restarted) when it drops."""

    def __init__(self):
        self.sock = None

    def connect(self):
        for attempt in range(2):
            for _ in range(50):
                try:
                    self.sock = socket.create_connection(('127.0.0.1', INJECTOR_PORT), timeout=2)
                    self.sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
                    return
                except OSError:
                    time.sleep(0.2)
            log('guest pose injector unreachable: restarting the guest helpers')
            start_guest()
        sys.exit('guest pose injector unreachable')

    def send(self, data):
        try:
            self.sock.sendall(data)
        except OSError:
            log('guest pose injector dropped; reconnecting')
            self.connect()
            self.sock.sendall(data)


class Session:
    def __init__(self, client, display, guest):
        self.client, self.display, self.guest = client, display, guest
        self.lock = threading.Lock()
        self.stop = False
        self.need_idr = True
        self.seq = int(time.time() * 1000) & 0x3FFFFFFF   # fresh numbers each run: the guest still shows the last ones
        self.times = {}          # pose number -> the headset's tracking time
        self.origin = None       # first head position: the guest's floor origin is put under it
        self.encoder = None
        self.status_at = 0

    def send(self, ptype, payload):
        with self.lock:
            self.client.sendall(bytes([ptype]) + struct.pack('<I', len(payload)) + payload)

    def recv_exact(self, n):
        buf = b''
        while len(buf) < n:
            chunk = self.client.recv(n - len(buf))
            if not chunk:
                raise ConnectionError('headset disconnected')
            buf += chunk
        return buf

    def configure(self, hello):
        log('HELLO:', json.dumps(hello))
        # x264 zerolatency: its stream tells the decoder not to hold frames back (VideoToolbox's doesn't, and the
        # Quest's decoder then sits on ~5 frames). Rate-capped so a busy frame can't stall the link.
        c = av.CodecContext.create('libx264', 'w')
        c.width, c.height, c.pix_fmt = CAP_W, ENC_H, 'yuv420p'
        c.time_base, c.framerate = Fraction(1, 1_000_000), Fraction(FPS, 1)
        c.bit_rate, c.gop_size, c.max_b_frames = BITRATE, 10 * FPS, 0
        kbps = BITRATE // 1000
        c.options = {'preset': 'ultrafast', 'tune': 'zerolatency',
                     'x264-params': f'vbv-maxrate={kbps}:vbv-bufsize={kbps // FPS * 2}:repeat-headers=1'}
        c.open()
        self.encoder = c
        self.need_idr = True
        config = {'codec': 'h264', 'eye_w': EYE_W, 'eye_h': ENC_H, 'fps': FPS, 'mic': False,
                  'pair': 'emuxr2', 'fov': FOV}
        self.send(2, json.dumps(config).encode())
        log('CONFIG:', json.dumps(config))

    def on_tracking(self, payload):
        t = qp.parse_tracking(payload)
        px, py, pz, qx, qy, qz, qw = t['head']
        if (qx, qy, qz, qw) == (0, 0, 0, 1) and t['time_ns'] == 0:
            return
        if self.origin is None:
            self.origin = (px, pz)
            log(f'head at {px:.2f} {py:.2f} {pz:.2f}: guest origin placed under it')
        self.seq += 1
        self.times[self.seq] = t['time_ns']
        self.times.pop(self.seq - 512, None)
        self.guest.send(struct.pack('<I7f', self.seq, px - self.origin[0], py, pz - self.origin[1], qx, qy, qz, qw))

    def reader(self):
        try:
            while not self.stop:
                hdr = self.recv_exact(5)
                ptype, plen = hdr[0], struct.unpack('<I', hdr[1:])[0]
                if plen > 8 << 20:
                    raise ConnectionError('oversize packet')
                payload = self.recv_exact(plen) if plen else b''
                if ptype == 1:
                    self.configure(json.loads(payload or b'{}'))
                elif ptype == 3:
                    self.on_tracking(payload)
                elif ptype == 7:
                    self.need_idr = True
                elif ptype == 9 and time.monotonic() - self.status_at > 5:
                    self.status_at = time.monotonic()
                    log('quest:', payload.decode('utf-8', 'replace')[:200])
        except (OSError, ValueError, ConnectionError) as e:
            log('reader ended:', e)
        finally:
            self.stop = True

    def run(self):
        threading.Thread(target=self.reader, daemon=True).start()
        last, sent_seq, sent_sig, sent_at, frame_id, n, t0, skipped = None, None, None, 0.0, 0, 0, time.monotonic(), {}
        padded = np.zeros((ENC_H, CAP_W), np.uint32)
        stamped_at = time.monotonic()
        while not self.stop:
            last, img = self.display.grab(last)
            if img is None:
                time.sleep(0.001)
                continue
            if time.monotonic() - stamped_at > 3:   # the capture ended (e.g. the runtime restarted): ask again
                log('no capture frames for 3 s: restarting the guest capture')
                start_guest(capture_only=True)
                stamped_at = time.monotonic()
            if self.encoder is None or img.shape != (CAP_H, CAP_W):
                continue
            seq = pose_number(img)
            now = time.monotonic()
            if seq is not None:
                stamped_at = now
            when = self.times.get(seq)
            sig = img[::16, ::16]   # the emulator display repeats frames: skip ones identical to the last sent
            why = 'unstamped' if seq is None else 'unknown pose' if when is None else \
                'repeat' if seq == sent_seq and np.array_equal(sig, sent_sig) else None
            if why and now - sent_at > 1 and seq is not None:
                # keep-alive (the client drops a silent link after 5 s): the newest pose's time, or 0 before any
                # tracking, which the client decodes but never shows
                why, when = None, self.times.get(self.seq, 0)
            if why:
                skipped[why] = skipped.get(why, 0) + 1
                continue
            padded[:CAP_H] = img
            padded[CAP_H - MARK:CAP_H, :MARK_BLOCKS * MARK] = 0
            frame = av.VideoFrame.from_ndarray(padded.view(np.uint8).reshape(ENC_H, CAP_W, 4), format='bgra')
            frame = frame.reformat(format='yuv420p')
            frame.pts = int(now * 1_000_000)
            if self.need_idr:
                self.need_idr = False
                frame.pict_type = av.video.frame.PictureType.I
            for pkt in self.encoder.encode(frame):
                header = struct.pack('<QQB', frame_id, when, 1 if pkt.is_keyframe else 0)
                self.send(4, header + bytes(pkt))
                frame_id += 1
            sent_seq, sent_sig, sent_at = seq, sig.copy(), now
            n += 1
            if now - t0 >= 5:
                log(f'video: {n / (now - t0):.1f} fps, pose {self.seq - seq} behind, skipped {skipped} ')
                n, t0, skipped = 0, now, {}


def main():
    display = Display()
    log(f'emulator framebuffer {display.w}x{display.h}')
    start_guest()
    guest = Guest()
    guest.connect()
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(('127.0.0.1', QUEST_PORT))
    srv.listen(1)
    quest = quest_serial()
    if quest:
        adb('reverse', f'tcp:{QUEST_PORT}', f'tcp:{QUEST_PORT}', serial=quest)
        log(f'headset {quest}: adb reverse tcp:{QUEST_PORT}')
    log(f'waiting for the headset on {QUEST_PORT}')
    while True:
        client, addr = srv.accept()
        client.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        log('headset connected')
        try:
            Session(client, display, guest).run()
        except OSError as e:
            log('session ended:', e)
        finally:
            client.close()
        log('headset gone; waiting for it to reconnect')


if __name__ == '__main__':
    main()
