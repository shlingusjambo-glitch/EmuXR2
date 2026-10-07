#!/usr/bin/env python3
"""Exercise a live EmuXR2 stream and injector. Does not verify headset comfort.

Use while the real Quest client is disconnected:
  python3 bridge/live_smoke.py --seconds 30 --controllers
"""
import argparse
import json
import math
import statistics
import socket
import struct
import threading
import time


def send(s, kind, body):
    s.sendall(struct.pack('<BI', kind, len(body)) + body)


def exact(s, n):
    b = bytearray()
    while len(b) < n:
        part = s.recv(n - len(b))
        if not part:
            raise EOFError('streamer disconnected')
        b.extend(part)
    return bytes(b)


def receive(s):
    kind, length = struct.unpack('<BI', exact(s, 5))
    if length > 8 << 20:
        raise ValueError('oversize packet')
    return kind, exact(s, length)


def tracking(timestamp, elapsed, controllers):
    q = (0, math.sin(.1 * math.sin(elapsed)), 0, math.cos(.1 * math.sin(elapsed)))
    pose = lambda x, y, z: struct.pack('<7f', x, y, z, *q)
    payload = struct.pack('<Q', timestamp) + pose(0, 1.6, 0)
    for x in (-.032, .032):
        payload += pose(x, 1.6, 0) + struct.pack('<4f', -.8, .8, .8, -.8)
    for h in range(2):
        trigger = .85 if int(elapsed * 2) % 2 else 0
        buttons = 16 if h == 0 and .5 < elapsed < .6 else 0
        handpose = pose((-.25, .25)[h], 1.25, -.5)
        payload += struct.pack('<II', 3 if controllers else 0, buttons) + handpose * 2
        payload += struct.pack('<4f', trigger, .2, .25 * math.sin(elapsed), .25 * math.cos(elapsed))
    assert len(payload) == 284
    return payload


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--seconds', type=float, default=15)
    parser.add_argument('--controllers', action='store_true')
    parser.add_argument('--tracking-hz', type=float, default=72)
    parser.add_argument('--max-p95-ms', type=float, default=500)
    parser.add_argument('--eye-w', type=int, default=1216)
    parser.add_argument('--eye-h', type=int, default=1344)
    args = parser.parse_args()
    with socket.create_connection(('127.0.0.1', 9945), timeout=10) as s:
        s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        hello = json.dumps(dict(device='live smoke', eye_w=args.eye_w, eye_h=args.eye_h,
                                refresh_rates=[72], codecs=['h264'], features=['fov'])).encode()
        framed = struct.pack('<BI', 1, len(hello)) + hello
        for chunk in (framed[:2], framed[2:8], framed[8:]):
            s.sendall(chunk)
        kind, body = receive(s)
        assert kind == 2
        config = json.loads(body)
        print('CONFIG', config, flush=True)
        stop = threading.Event()
        sent = set()
        errors = []
        start = time.monotonic()
        def samples():
            try:
                while not stop.is_set():
                    timestamp = time.monotonic_ns()
                    sent.add(timestamp)
                    send(s, 3, tracking(timestamp, time.monotonic() - start, args.controllers))
                    stop.wait(1/args.tracking_hz)
            except OSError as e:
                if not stop.is_set():
                    errors.append(e)
        worker = threading.Thread(target=samples)
        worker.start()
        ages = []
        frames = 0
        last_id = -1
        requested = None
        recovered = False
        capture_seen = False
        try:
            while time.monotonic() - start < args.seconds:
                kind, body = receive(s)
                assert kind == 4 and len(body) > 17
                frame_id, timestamp, flags = struct.unpack('<QQB', body[:17])
                assert frame_id > last_id
                last_id = frame_id
                if timestamp == 0 and not capture_seen:
                    continue  # startup diagnostic image before first tracking/capture pair
                assert timestamp in sent, f'pose timestamp not sent by this session: {timestamp}'
                capture_seen = True
                ages.append((time.monotonic_ns() - timestamp) / 1e6)
                frames += 1
                if frames == 3:
                    requested = frame_id
                    send(s, 7, b'')
                elif requested is not None and frame_id > requested and flags & 1:
                    recovered = True
            assert not errors, errors
            assert frames >= args.seconds * 10, f'insufficient real captured frames: {frames}'
            assert recovered, 'IDR recovery failed'
            p95 = sorted(ages)[int((len(ages)-1) * .95)]
            assert p95 < args.max_p95_ms, f'pose backlog: p95 {p95:.1f} ms'
            print(f'PASS: {frames} captured frames in {time.monotonic()-start:.1f}s; '
                  f'pose age median {statistics.median(ages):.1f} ms / p95 {p95:.1f} ms; '
                  'exact timestamps, sequential IDs, fragmented HELLO, IDR recovery', flush=True)
        finally:
            stop.set()
            s.shutdown(socket.SHUT_RDWR)
            worker.join(timeout=3)


if __name__ == '__main__':
    main()
