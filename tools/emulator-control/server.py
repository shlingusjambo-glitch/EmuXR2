#!/usr/bin/env python3
"""Loopback-only Horizon display/input controller. Requires imageio-ffmpeg and adb."""
import ctypes, mmap, struct
import base64, hashlib, select, socket
import numpy as np
import av
import argparse, atexit, io, json, math, os, secrets, signal, subprocess, threading, time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import urlparse, parse_qs

ROOT = Path(__file__).resolve().parents[2]
ADB = Path(os.environ.get('ANDROID_SDK_ROOT', Path.home() / 'Library/Android/sdk')) / 'platform-tools/adb'
TOKEN = secrets.token_urlsafe(24)
frame = b''
frame_id = 0
status = 'Connecting'
condition = threading.Condition()
stop = threading.Event()
input_lock = threading.Lock()
held = set()
last_input = time.monotonic()
pose_lock = threading.Lock()
pose = (0., 0.)
pose_dirty = threading.Event()
bridge = None
pointer_bridge = None

def adb(*args, timeout=8):
    return subprocess.run([str(ADB), *args], capture_output=True, timeout=timeout, check=True).stdout

def release():
    global last_input
    with input_lock:
        if bridge and bridge.poll() is None:
            bridge.stdin.write('u\n' + ''.join(f'k {k} 0\n' for k in held))
            bridge.stdin.flush()
        if pointer_bridge and pointer_bridge.poll() is None:
            pointer_bridge.stdin.write('u\n'); pointer_bridge.stdin.flush()
        held.clear()
        last_input = time.monotonic()

def send(commands):
    global last_input
    with input_lock:
        if bridge.poll() is not None:
            raise RuntimeError('Input disconnected; restart the controller')
        for c in commands:
            if c[0] == 'k':
                if c[2]: held.add(c[1])
                else: held.discard(c[1])
            target = pointer_bridge if (c[0] == 'p' and pointer_bridge) else bridge
            target.stdin.write(' '.join(map(str, c)) + '\n')
            if target is pointer_bridge: target.stdin.flush()
        bridge.stdin.flush()
        last_input = time.monotonic()

video_lock=threading.Lock()
def video(handler):
    global status
    from fractions import Fraction
    key=handler.headers.get('Sec-WebSocket-Key')
    if not key or handler.headers.get('Origin') not in {f'http://127.0.0.1:{handler.server.server_port}',f'http://localhost:{handler.server.server_port}'}:
        return handler.reply(403,b'{}')
    handler.send_response(101); handler.send_header('Upgrade','websocket');handler.send_header('Connection','Upgrade')
    handler.send_header('Sec-WebSocket-Accept',base64.b64encode(hashlib.sha1((key+'258EAFA5-E914-47DA-95CA-C5AB0DC85B11').encode()).digest()).decode());handler.end_headers()
    if not video_lock.acquire(blocking=False):
        handler.connection.close(); return
    memory=None
    try:
        name=adb('emu','screenrecord','webrtc','start','60').decode().splitlines()[0].strip()
        fd=ctypes.CDLL(None).shm_open(name.encode(),os.O_RDONLY,0)
        if fd<0:raise RuntimeError('Video memory unavailable')
        memory=mmap.mmap(fd,os.fstat(fd).st_size,access=mmap.ACCESS_READ);os.close(fd)
        w,h=struct.unpack('<2I',memory[:8])
        codec=av.CodecContext.create('libvpx','w');codec.width=720;codec.height=810
        codec.pix_fmt='yuv420p';codec.time_base=Fraction(1,1000000);codec.framerate=Fraction(60,1)
        codec.bit_rate=4000000;codec.thread_count=4
        codec.options={'deadline':'realtime','cpu-used':'8','lag-in-frames':'0','g':'60'};codec.open()
        start=time.monotonic();next_time=start;count=0;last_report=start
        handler.connection.settimeout(1)
        while not stop.is_set():
            if select.select([handler.connection],[],[],0)[0]:
                data=handler.connection.recv(4096)
                if not data or data[0]&15==8:break
            now=time.monotonic()
            if now<next_time:stop.wait(next_time-now)
            next_time=max(next_time+1/60,time.monotonic())
            pixels=np.ndarray((h,w,4),dtype=np.uint8,buffer=memory,offset=24)
            upright=np.rot90(pixels,-1) if h>w else pixels
            eye=np.ascontiguousarray(upright[:,:upright.shape[1]//2])
            frame=av.VideoFrame.from_ndarray(eye,format='bgra').reformat(width=720,height=810,format='yuv420p')
            frame.pts=int((time.monotonic()-start)*1000000)
            for packet in codec.encode(frame):
                payload=bytes([int(packet.is_keyframe)])+struct.pack('>Q',packet.pts)+bytes(packet)
                n=len(payload);header=b'\x82'+(bytes([n]) if n<126 else b'\x7e'+struct.pack('>H',n) if n<65536 else b'\x7f'+struct.pack('>Q',n))
                handler.connection.sendall(header+payload);count+=1
            del pixels,upright
            now=time.monotonic()
            if now-last_report>=1:
                status=f'Connected · {round(count/(now-last_report))} fps';count=0;last_report=now
    except (BrokenPipeError,ConnectionResetError,TimeoutError,OSError):pass
    finally:
        if memory:memory.close()
        video_lock.release()

def watchdog():
    while not stop.wait(.5):
        if time.monotonic()-last_input>3:
            try: release()
            except Exception: pass

def poses():
    while not stop.is_set():
        if not pose_dirty.wait(.2): continue
        pose_dirty.clear()
        with pose_lock: yaw, pitch = pose
        # Rotation Y(yaw) * X(pitch), quaternion x,y,z,w.
        sy, cy = math.sin(yaw/2), math.cos(yaw/2)
        sp, cp = math.sin(pitch/2), math.cos(pitch/2)
        q = (cy*sp, sy*cp, -sy*sp, cy*cp)
        values = ' '.join(f'{x:.8f}' for x in q)
        try:
            adb('shell', f"printf '%s\\n' '{values}' > /data/local/tmp/macvr-head.new && mv /data/local/tmp/macvr-head.new /data/local/tmp/macvr-head")
        except Exception: pass
        stop.wait(.08)

def mjpeg(handler):
    global status
    from PIL import Image
    if not video_lock.acquire(blocking=False):
        return handler.reply(503, b'{"error":"Stream busy"}')
    memory = None
    try:
        handler.send_response(200)
        handler.send_header('Content-Type', 'multipart/x-mixed-replace; boundary=--frame')
        handler.send_header('Cache-Control', 'no-store, no-cache, must-revalidate, pre-check=0, post-check=0, max-age=0')
        handler.send_header('Pragma', 'no-cache')
        handler.send_header('Connection', 'close')
        handler.end_headers()

        name = adb('emu', 'screenrecord', 'webrtc', 'start', '60').decode().splitlines()[0].strip()
        fd = ctypes.CDLL(None).shm_open(name.encode(), os.O_RDONLY, 0)
        if fd < 0: raise RuntimeError('Video memory unavailable')
        memory = mmap.mmap(fd, os.fstat(fd).st_size, access=mmap.ACCESS_READ)
        os.close(fd)
        w, h = struct.unpack('<2I', memory[:8])

        start = time.monotonic()
        next_time = start
        count = 0
        last_report = start
        while not stop.is_set():
            now = time.monotonic()
            if now < next_time: stop.wait(next_time - now)
            next_time = max(next_time + 1/60, time.monotonic())
            pixels = np.ndarray((h, w, 4), dtype=np.uint8, buffer=memory, offset=24)
            upright = np.rot90(pixels, -1) if h > w else pixels
            eye = np.ascontiguousarray(upright[:, :upright.shape[1]//2, :3])
            # Eye is BGRA -> convert to RGB for JPEG
            img = Image.fromarray(eye[:, :, ::-1])
            buf = io.BytesIO()
            img.save(buf, format='JPEG', quality=75)
            data = buf.getvalue()
            part = (
                b'--frame\r\n'
                b'Content-Type: image/jpeg\r\n'
                b'Content-Length: ' + str(len(data)).encode() + b'\r\n\r\n' +
                data + b'\r\n'
            )
            handler.wfile.write(part)
            handler.wfile.flush()
            count += 1
            now = time.monotonic()
            if now - last_report >= 1:
                status = f'Connected · {round(count/(now-last_report))} fps (MJPEG)'
                count = 0
                last_report = now
    except (BrokenPipeError, ConnectionResetError, OSError): pass
    finally:
        if memory: memory.close()
        video_lock.release()

class Handler(BaseHTTPRequestHandler):
    def log_message(self, fmt, *args): pass
    def valid_host(self):
        return self.headers.get('Host') in {f'127.0.0.1:{self.server.server_port}', f'localhost:{self.server.server_port}'}
    def reply(self, code, data, kind='application/json'):
        self.send_response(code); self.send_header('Content-Type', kind)
        self.send_header('Cache-Control', 'no-store'); self.send_header('X-Content-Type-Options', 'nosniff')
        self.send_header('Content-Length', str(len(data))); self.end_headers(); self.wfile.write(data)
    def do_GET(self):
        if not self.valid_host(): return self.reply(403, b'{}')
        p = urlparse(self.path)
        if p.path == '/':
            html = (Path(__file__).with_name('index.html').read_text().replace('__TOKEN__', TOKEN)).encode()
            return self.reply(200, html, 'text/html; charset=utf-8')
        if parse_qs(p.query).get('token') != [TOKEN]: return self.reply(403, b'{}')
        if p.path == '/status': return self.reply(200, json.dumps({'status':status, 'input':bridge.poll() is None}).encode())
        if p.path == '/stream.mjpeg': return mjpeg(self)
        if p.path != '/video': return self.reply(404,b'{}')
        video(self)
    def do_POST(self):
        global pose
        origin = self.headers.get('Origin')
        expected = {f'http://127.0.0.1:{self.server.server_port}', f'http://localhost:{self.server.server_port}'}
        if not self.valid_host() or self.headers.get('X-Control-Token') != TOKEN or (origin and origin not in expected):
            return self.reply(403, b'{}')
        try:
            n = int(self.headers.get('Content-Length', 0))
            if not 0 < n <= 16384: raise ValueError('Invalid body size')
            value = json.loads(self.rfile.read(n))
            if self.path == '/release': release()
            elif self.path == '/pose':
                yaw, pitch = float(value['yaw']), float(value['pitch'])
                if not math.isfinite(yaw) or not math.isfinite(pitch): raise ValueError('Invalid angle')
                with pose_lock: pose = (max(-math.pi*4,min(math.pi*4,yaw)), max(-1.4,min(1.4,pitch)))
                pose_dirty.set()
            elif self.path == '/input':
                commands = value['commands']
                if not isinstance(commands,list) or len(commands)>100: raise ValueError('Invalid commands')
                for c in commands:
                    if not isinstance(c,list) or not c: raise ValueError('Invalid command')
                    if c[0] in ('d','u','c','r') and len(c)==1: continue
                    if c[0]=='m' and len(c)==3 and all(type(x)==int and abs(x)<=1000 for x in c[1:]): continue
                    if c[0]=='p' and len(c)==3 and all(type(x)==int for x in c[1:]) and 0<=c[1]<=1919 and 0<=c[2]<=1079: continue
                    if c[0]=='w' and len(c)==2 and type(c[1]) in (int,float) and math.isfinite(c[1]) and abs(c[1])<=20: continue
                    if c[0]=='k' and len(c)==3 and type(c[1])==int and 1<=c[1]<=248 and type(c[2])==int and c[2] in (0,1): continue
                    raise ValueError('Invalid input command')
                send(commands)
            else: return self.reply(404,b'{}')
            self.reply(200,b'{"ok":true}')
        except Exception as e: self.reply(400,json.dumps({'error':str(e)}).encode())

def cleanup():
    stop.set()
    try: release()
    except Exception: pass
    if pointer_bridge:
        pointer_bridge.stdin.close()
        try: pointer_bridge.wait(timeout=2)
        except subprocess.TimeoutExpired: pointer_bridge.terminate()
    if bridge:
        bridge.stdin.close()
        try: bridge.wait(timeout=2)
        except subprocess.TimeoutExpired: bridge.terminate()
    try: adb('shell','rm -f /data/local/tmp/macvr-head /data/local/tmp/macvr-head.new',timeout=2)
    except Exception: pass

if __name__=='__main__':
    parser=argparse.ArgumentParser(); parser.add_argument('--port',type=int,default=8766); args=parser.parse_args()
    subprocess.run([str(ROOT/'input/build.sh')],check=True)
    subprocess.run([str(ROOT/'hot.sh'),str(ROOT/'compat/pose.sh'),'/system/etc/macvr-pose.sh'],check=True)
    adb('shell','setprop ctl.restart macvr-pose')
    adb('push',str(ROOT/'input/out/vinput'),'/data/local/tmp/vinput')
    adb('shell','chmod 755 /data/local/tmp/vinput; rm -f /data/local/tmp/macvr-head')
    bridge=subprocess.Popen([str(ADB),'shell','-T','/data/local/tmp/vinput'],stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True,bufsize=1)
    if bridge.stdout.readline().strip()!='ready': raise RuntimeError('Guest input device failed to start')
    subprocess.run([str(ROOT/'input/build-pointer.sh')],check=True)
    adb('push',str(ROOT/'input/out/pointer-dex/classes.dex'),'/data/local/tmp/pointer.dex')
    adb('push',str(ROOT/'input/out/system-input'),'/data/local/tmp/system-input')
    pointer_bridge=subprocess.Popen([str(ADB),'shell','-T','CLASSPATH=/data/local/tmp/pointer.dex /data/local/tmp/system-input app_process /system/bin AbsolutePointer'],stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=open('/tmp/emuxr-pointer.log','a'),text=True,bufsize=1)
    if pointer_bridge.stdout.readline().strip()!='ready': raise RuntimeError('Absolute pointer failed to start')
    server=ThreadingHTTPServer(('127.0.0.1',args.port),Handler); server.daemon_threads=True
    atexit.register(cleanup)
    for target in (watchdog,poses): threading.Thread(target=target,daemon=True).start()
    signal.signal(signal.SIGTERM,lambda *_: (_ for _ in ()).throw(KeyboardInterrupt()))
    print(f'Emulator controller: http://127.0.0.1:{args.port}',flush=True)
    try: server.serve_forever()
    except KeyboardInterrupt: pass
    finally: server.server_close()
