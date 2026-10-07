#!/usr/bin/env python3
"""EmuXR2 -> Quest streaming (the VR4Mac client, com.vr4mac.client, protocol in VR4Mac's common/vr4mac.h).

Frames: Meta's compositor's own display output, read from the emulator's shared framebuffer. Its lens-distortion mesh
is replaced by a flat one (bridge/flat_mesh.py), so each half of the display is that eye's plain perspective view over
exactly the Quest 2's field of view, composited and timewarped by Horizon as on a headset. Each frame carries the number
of the head pose it was drawn with (stamped by the EGL shim), so its VIDEO packet names the headset's exact tracking
sample and the headset reprojects it correctly. Head poses go straight to the guest's injector (input/Injector.java) over
adb forward, one per TRACKING packet.

Usage: stream.py   (emulator running, Quest on USB with the VR4Mac client; needs PyAV with VideoToolbox)
"""
import ctypes
import functools
import json
import math
import mmap
import os
import queue
import socket
import struct
import subprocess
import sys
import threading
import time
from fractions import Fraction

import av
import pixels
import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import flat_mesh
import quest_proto as qp
import unknown_sources

ADB = os.environ.get('ANDROID_SDK_ROOT', os.path.expanduser('~/Library/Android/sdk')) + '/platform-tools/adb'
EMU = os.environ.get('EMUXR2_SERIAL', 'emulator-5554')
HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, '..', 'input', 'out')
INJECTOR_PORT, QUEST_PORT, AUDIO_PORT = 7791, 9945, 7793
AUDIO_CHUNK = 1920   # 10 ms of 48 kHz stereo s16le (common/vr4mac.h VR4_AUDIO)

# The display: both eyes side by side, 1280x1600 each (the emulator's panel is 2560x1600).
CAP_W, CAP_H, EYE_W = 2560, 1600, 1280
ENC_H = CAP_H   # encoded height: 16-aligned (the Quest 1 AVC decoder drops frames otherwise)
# Horizon's raw tracking space has its floor at y = -1.675 without a Guardian (the runtime logs RawFloorHeight); the
# Quest's stage space has it at 0. Injected heights are moved onto the guest's floor so eye height stays true.
GUEST_FLOOR = float(os.environ.get('EMUXR2_GUEST_FLOOR', -1.675))
# Horizon reports a Touch controller's OpenXR grip pose as IMU pose * GRIP_FROM_IMU (measured with
# bridge/calibrate_poses.py: 60 degrees about x, 3 cm down, 4 cm back); the headset's view pose is its IMU pose. The
# Quest's grip poses are injected as grip * GRIP_FROM_IMU^-1 so the guest's controllers land where the real ones are.
GRIP_FROM_IMU = ((0.0, -0.03, 0.04), (math.sin(math.radians(30)), 0.0, 0.0, math.cos(math.radians(30))))
# Each eye's field of view (degrees left, right, up, down): the Quest 2's, which Horizon renders its eye buffers with and
# the flat mesh spreads over each half of the display (bridge/calibrate_display.py measures it: within 0.5 %).
EYE_FOV = ((49, 45, 48, 50), (45, 49, 48, 50))
# CONFIG uses XrFovf angles in radians, not projection tangents. The client takes tan() when drawing.
FOV = [[-math.radians(l), math.radians(r), math.radians(u), -math.radians(d)]
       for l, r, u, d in EYE_FOV]
MESH = '/data/local/tmp/emuxr2-mesh.bin'
MARK_BLOCKS, MARK = 36, 8   # pose-number stamp: 36 blocks of 8x8 px along the bottom-left edge
FPS, BITRATE = 60, 40_000_000
DEFAULT_PROF = {'codec': 'h264', 'eye_w': EYE_W, 'eye_h': ENC_H,
                'fps': FPS, 'bitrate': BITRATE, 'device': 'default'}
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


def wait_injector(timeout=25):
    """Wait until the guest injector has bound its port (its JVM takes seconds to boot).

    Without this the adb forward accepts the host connection while the guest leg is
    refused, leaving a socket that connects but never reaches the injector."""
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        r = adb('shell', 'grep -q "Injector: listening" /data/local/tmp/injector.log')
        if r.returncode == 0:
            return True
        time.sleep(0.5)
    return False


def prepare_library():
    """Use Horizon's native offline Library when no account has been added."""
    if os.environ.get('EMUXR2_LOCAL_ACCOUNT', 'auto') == '0':
        return
    check = adb('shell', 'dumpsys account')
    if check.returncode == 0 and 'Account {' not in check.stdout:
        adb('shell', 'oculuspreferences --setc local_account_mode_enabled_v1 true')


def restart_runtime():
    """Restart Meta's VR runtime (its compositor); VrShell and ShellEnv come back with it in ~20 s."""
    adb('shell', 'kill $(pidof com.oculus.vrruntimeservice)')


def flat_display():
    """Make sure the compositor draws plain eye images (the flat mesh); restarts the runtime when it had another."""
    with open(os.path.join(OUT, 'emuxr2-mesh.bin'), 'wb') as f:
        f.write(flat_mesh.mesh(EYE_FOV))
    adb('push', os.path.join(OUT, 'emuxr2-mesh.bin'), MESH)
    if adb('shell', 'getprop debug.oculus.distortionFileName').stdout.strip() != MESH:
        adb('shell', f'chmod 644 {MESH}; setprop debug.oculus.distortionFileName {MESH}')
        log('compositor: flat display mesh installed, restarting the VR runtime')
        restart_runtime()


def start_guest():
    """(Re)start the pose injector and audio in the guest; their dex files come from input/build.sh."""
    # the guest has no proximity sensor to wake it: asleep, it composes nothing and the stream freezes
    adb('shell', 'dumpsys battery set ac 1; svc power stayon true; input keyevent WAKEUP')
    # Each pose otherwise emits many debug lines from Meta's injection service (~700/s).
    # Keep warnings/errors; profiling can restore the detailed tracking diagnostics.
    tracking_log = 'D' if os.getenv('EMUXR2_PROFILE') == '1' else 'W'
    adb('shell', f'setprop log.tag.TrackingDataInjection {tracking_log}')
    # no room to guard: Guardian only retries spatial anchors (~15/s), burning CPU and leaking memory
    adb('shell', 'setprop persist.oculus.guardian_disable 1')
    # the brightness slider dims the stream from full: start it at full once (Horizon's default is a third)
    adb('shell', '[ "$(getprop persist.emuxr2.brightness)" = 1 ] || '
                 '{ settings put system screen_brightness_for_vr 255; setprop persist.emuxr2.brightness 1; }')
    prepare_library()
    unknown_sources.main()   # sideloaded games in the Library's Unknown Sources without a Meta account
    flat_display()
    for dex in ('injector', 'audio'):
        adb('push', f'{OUT}/{dex}.dex', f'/data/local/tmp/{dex}.dex')
    # the casting capture earlier versions streamed: it only costs the compositor a second render now
    adb('shell', 'for p in $(pgrep -f "[a]pp_process.*(Injector|Capture|Audio)"); do kill $p; done')
    time.sleep(0.5)
    adb('shell', 'rm -f /data/local/tmp/injector.log')
    # Only one head-pose producer may own TrackingDataInjection.
    adb('shell', 'setprop ctl.stop macvr-pose')
    adb('shell', 'CLASSPATH=/data/local/tmp/injector.dex setsid nohup app_process /system/bin Injector '
                 '> /data/local/tmp/injector.log 2>&1 < /dev/null &')
    adb('shell', 'CLASSPATH=/data/local/tmp/audio.dex setsid nohup app_process / Audio '
                 '> /data/local/tmp/audio.log 2>&1 < /dev/null &')
    adb('forward', f'tcp:{AUDIO_PORT}', f'tcp:{AUDIO_PORT}')
    adb('forward', f'tcp:{INJECTOR_PORT}', f'tcp:{INJECTOR_PORT}')
    if not wait_injector():
        log('warning: guest pose injector not listening yet; poses will reconnect on first send')


class Display:
    """The emulator's shared framebuffer: header (width, height, fps, frame counter, ...) then BGRA pixels."""

    def __init__(self):
        result = adb('emu', 'screenrecord', 'webrtc', 'start', '72')
        names = [line.strip() for line in result.stdout.splitlines() if line.strip().startswith('videmulator')]
        if result.returncode or not names:
            raise RuntimeError('cannot start emulator framebuffer capture: ' + result.stdout + result.stderr)
        fd = ctypes.CDLL(None).shm_open(names[0].encode(), os.O_RDONLY, 0)
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
        img = self.snapshot()
        if self.counter() != n:   # written while copying: take the next one
            return last, None
        return n, img

    def snapshot(self):
        """Own one upright snapshot; rotating the shared view before copying avoids a second full-frame copy."""
        return pixels.snapshot(self.px)

    def current(self):
        """The current upright image regardless of the frame counter, or None."""
        img = self.snapshot()
        return img if img.shape == (CAP_H, CAP_W) else None


def pose_number(img):
    """The stamped pose number, or None when this isn't a capture frame."""
    row = img[CAP_H - MARK // 2, MARK // 2:MARK_BLOCKS * MARK:MARK]
    bits = ((row >> 8) & 0xFF) > 127   # green channel
    if list(bits[:4]) != [True, False, True, True]:
        return None
    return sum(int(b) << i for i, b in enumerate(bits[4:]))


def hide_stamp(frame):
    """Cover the pose-number stamp with the row above it (black there shows when the headset stretches the edge)."""
    frame[CAP_H - MARK:CAP_H, :MARK_BLOCKS * MARK] = frame[CAP_H - MARK - 1, :MARK_BLOCKS * MARK]


CLIENT = 'com.vr4mac.client'
CLIENT_APK = os.environ.get('EMUXR2_CLIENT_APK', os.path.join(HERE, '..', '..', 'android', 'app', 'build', 'outputs',
                                                              'apk', 'debug', 'app-debug.apk'))


def update_client():
    """Replace an outdated headset client (one that can't show frames at their own field of view, so the world looks
    magnified) with the one built from this repository, over USB, and start it."""
    quest = quest_serial()
    if not quest or not os.path.exists(CLIENT_APK):
        log('headset client is outdated (no field-of-view support: the view looks zoomed in); install', CLIENT_APK)
        return
    log(f'headset client is outdated: installing {CLIENT_APK} on {quest}')
    r = adb('install', '-r', '-g', CLIENT_APK, serial=quest, timeout=120)
    if 'INSTALL_FAILED_UPDATE_INCOMPATIBLE' in r.stdout + r.stderr:   # signed with another key: replace it
        adb('uninstall', CLIENT, serial=quest, timeout=60)
        r = adb('install', '-g', CLIENT_APK, serial=quest, timeout=120)
    if 'Success' not in r.stdout:
        log('client update failed:', (r.stdout + r.stderr).strip()[-300:])
        return
    adb('reverse', f'tcp:{QUEST_PORT}', f'tcp:{QUEST_PORT}', serial=quest)
    adb('shell', f'am start -n {CLIENT}/.MainActivity', serial=quest)
    log('headset client updated and restarted')


class Guest:
    """Latest-pose mailbox: guest reconnection must never block headset packet reads."""

    def __init__(self):
        self.sock = None
        self.pending = queue.Queue(maxsize=1)
        self.stopping = threading.Event()
        self.worker = None

    def connect(self):
        self.sock = socket.create_connection(('127.0.0.1', INJECTOR_PORT), timeout=1)
        self.sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        if self.worker is None:
            self.worker = threading.Thread(target=self._pump, daemon=True)
            self.worker.start()

    def send(self, data):
        try:
            self.pending.put_nowait(data)
        except queue.Full:
            try:
                self.pending.get_nowait()
            except queue.Empty:
                pass
            self.pending.put_nowait(data)

    def _pump(self):
        failures = 0
        while not self.stopping.is_set():
            try:
                data = self.pending.get(timeout=.1)
            except queue.Empty:
                continue
            try:
                if self.sock is None:
                    self.connect()
                    # Discard the pose held while reconnecting in favor of the latest.
                    try:
                        data = self.pending.get_nowait()
                    except queue.Empty:
                        pass
                self.sock.sendall(data)
                failures = 0
            except OSError as e:
                if self.sock is not None:
                    self.sock.close()
                    self.sock = None
                failures += 1
                if failures == 1:
                    log('guest injector disconnected:', e)
                if failures >= 10:
                    log('restarting guest injector after repeated connection failures')
                    start_guest()
                    failures = 0
                self.stopping.wait(.2)

    def close(self):
        self.stopping.set()
        if self.sock is not None:
            try:
                self.sock.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass
        if self.worker is not None:
            self.worker.join(timeout=2)
        if self.sock is not None:
            self.sock.close()


# Meta's injection service's button ids, as bits
TRIGGER, HOME, BACK, STICK, GRIP, AX, BY, THUMBREST = (1 << i for i in range(8))
# The EGL shim emulates float buffer textures through shader storage buffers,
# allowing the guest controller models and cursor renderer to run on ANGLE ES 3.1.
# EMUXR2_CONTROLLERS=0 opts out.
CONTROLLERS = os.environ.get('EMUXR2_CONTROLLERS') != '0'


def imu_from_grip(position, orientation):
    """The controller IMU pose that Horizon turns into this grip pose."""
    tp, tq = GRIP_FROM_IMU
    q = qp._quat_mul(orientation, qp._quat_conj(tq))
    off = qp._rot_vec(q, tp)
    return tuple(position[i] - off[i] for i in range(3)), q


def touch_controller(hand, left, menu):
    """One Quest controller as (flags, pressed, touched, trigger, grip, stick x, stick y) for the guest. menu is
    retained for compatibility. The left menu button directly drives guest Home."""
    b = hand['buttons']
    active = hand['flags'] & qp.HAND_ACTIVE and hand['flags'] & 2 and not hand['flags'] & 4   # posed, not a bare hand
    pressed = touched = 0
    if hand['trigger'] > 0.6: pressed |= TRIGGER
    if hand['trigger'] > 0.05 or b & 64: touched |= TRIGGER
    if hand['squeeze'] > 0.6: pressed |= GRIP
    if hand['squeeze'] > 0.05: touched |= GRIP
    if b & (qp.BTN_X if left else qp.BTN_A): pressed |= AX
    if b & (qp.BTN_Y if left else qp.BTN_B): pressed |= BY
    if b & qp.BTN_STICK_CLICK: pressed |= STICK
    if b & 256: touched |= STICK
    if b & 128: touched |= THUMBREST
    if left and b & qp.BTN_MENU:
        pressed |= HOME
    if not active or not CONTROLLERS:
        return (0, 0, 0, 0.0, 0.0, 0.0, 0.0)
    return (1 if active and CONTROLLERS else 0, pressed, touched | pressed, hand['trigger'], hand['squeeze'], *hand['stick'])


def negotiate(hello):
    """Encode profile + client CONFIG for a HELLO dict; raises ValueError on bad HELLO.

    The encoded frame is always eye_w*2 x eye_h (what CONFIG advertises, what the
    headset's decoder surface is): the guest's native 960x1088 eyes, H.264;
    refresh is capped at the reported headset rate and 72 Hz.
    """
    prof = dict(DEFAULT_PROF)
    prof.update(qp.device_profile(hello))
    # the guest's own pixels: upscaling to the headset's eye size adds host encode and resample time (latency) and no
    # detail; the headset's compositor scales the layer anyway
    prof['eye_w'], prof['eye_h'] = EYE_W, ENC_H
    requested = int(os.environ.get('EMUXR2_FPS', prof['fps']))
    if not 30 <= requested <= 72:
        raise ValueError('EMUXR2_FPS must be between 30 and 72')
    prof['fps'] = min(prof['fps'], requested)
    config = {'codec': prof['codec'], 'eye_w': prof['eye_w'], 'eye_h': prof['eye_h'],
              'fps': prof['fps'], 'mic': False, 'pair': 'emuxr2', 'fov': FOV}
    return prof, config


@functools.lru_cache(maxsize=8)
def dim_tables(gain):
    """Lookup tables dimming limited-range YUV by gain: luma above black (16), chroma around neutral (128)."""
    v = np.arange(256, dtype=np.float32)
    return (np.clip(16 + (v - 16) * gain, 0, 255).astype(np.uint8), np.clip(128 + (v - 128) * gain, 0, 255).astype(np.uint8))


def brightness_gain():
    """The stream's gain for Horizon's brightness slider (screen_brightness_for_vr, 0-255): 20 % to 100 %."""
    r = adb('shell', 'settings get system screen_brightness_for_vr')
    try:
        return round(0.2 + 0.8 * min(255, max(0, int(r.stdout.strip()))) / 255, 2)
    except ValueError:
        return 1.0


class Session:
    def __init__(self, client, display, guest):
        self.client, self.display, self.guest = client, display, guest
        self.audio_thread = None
        self.gain = 1.0
        self.lock = threading.Lock()
        self.stop = False
        self.need_idr = True
        self.prof = dict(DEFAULT_PROF)
        self.frame_id = 0
        self.seq = int(time.time() * 1000) & 0x3FFFFFFF   # fresh numbers each run: the guest still shows the last ones
        self.times = {}          # pose number -> the headset's tracking time
        self.origin = None       # first head position: the guest's floor origin is put under it
        self.menu = [0, 0]       # left menu button: held since, Back pulse until
        self.encoder = None
        self.reformatter = av.video.reformatter.VideoReformatter()
        self.encoder_times = {}
        self.encoder_pts = 0
        self.status_at = 0
        self.hellos = queue.Queue()
        self.profile = os.environ.get('EMUXR2_PROFILE') == '1'
        self.profile_samples = []
        self.pose_profile = []
        self.pose_profile_at = 0

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
        try:
            prof, config = negotiate(hello)
        except ValueError as e:
            log('bad HELLO:', e)
            return
        if self.encoder is not None:
            try:
                self.encoder.close()
            except Exception:
                pass
        # macOS hardware encoding frees CPU for the guest. Baseline, one reference
        # and LOW_DELAY are required together to avoid holding multiple input frames.
        requested = os.getenv('EMUXR2_ENCODER', 'auto')
        hardware = sys.platform == 'darwin' and requested != 'software'
        def encoder(hardware):
            c = av.CodecContext.create('h264_videotoolbox' if hardware else 'libx264', 'w')
            c.width, c.height, c.pix_fmt = prof['eye_w'] * 2, prof['eye_h'], 'nv12' if hardware else 'yuv420p'
            c.time_base, c.framerate = Fraction(1, 1_000_000), Fraction(prof['fps'], 1)
            c.bit_rate, c.gop_size, c.max_b_frames = prof['bitrate'], 10 * prof['fps'], 0
            if hardware:
                c.flags |= av.codec.context.Flags.low_delay
                c.options = {'realtime': '1', 'prio_speed': '1', 'power_efficient': '0',
                             'allow_sw': '0', 'profile': 'baseline', 'max_ref_frames': '1'}
            else:
                kbps = prof['bitrate'] // 1000
                c.options = {'preset': 'ultrafast', 'tune': 'zerolatency',
                             'x264-params': f'vbv-maxrate={kbps}:vbv-bufsize={kbps // prof["fps"] * 2}:repeat-headers=1'}
            c.open()
            return c
        try:
            c = encoder(hardware)
        except (av.error.FFmpegError, ValueError) as error:
            if not hardware: raise
            log('hardware encoder unavailable; using software:', error)
            c = encoder(False)
        log('encoder:', c.name)
        self.encoder_times.clear()
        self.encoder = c
        self.prof = prof
        self.need_idr = True
        self.send(2, json.dumps(config).encode())
        log('CONFIG:', json.dumps(config))
        if 'fov' not in hello.get('features', ()) and not getattr(Session, 'updating', None):
            Session.updating = threading.Thread(target=update_client, daemon=True)   # once per streamer run
            Session.updating.start()
        if hello.get('audio') and not (self.audio_thread and self.audio_thread.is_alive()):
            self.audio_thread = threading.Thread(target=self.audio, daemon=True)
            self.audio_thread.start()

    def on_tracking(self, payload):
        t = qp.parse_tracking(payload)
        px, py, pz, qx, qy, qz, qw = t['head']
        if (qx, qy, qz, qw) == (0, 0, 0, 1) and t['time_ns'] == 0:
            return
        if self.profile:
            head_q = np.asarray((qx, qy, qz, qw))
            for offset in (36, 80):
                eye_q = np.asarray(struct.unpack_from('<7f', payload, offset)[3:])
                norm = np.linalg.norm(head_q) * np.linalg.norm(eye_q)
                if norm > 0:
                    self.pose_profile.append(math.degrees(2 * math.acos(min(1, abs(float(head_q @ eye_q)) / norm))))
            now = time.monotonic()
            if now - self.pose_profile_at >= 5 and self.pose_profile:
                log('head/eye orientation disagreement degrees median/max:',
                    round(float(np.median(self.pose_profile)), 3), round(max(self.pose_profile), 3))
                self.pose_profile.clear()
                self.pose_profile_at = now
        if self.origin is None:
            self.origin = (px, pz)
            log('headset eye fields of view (left right up down, degrees):',
                [[round(math.degrees(a)) for a in f] for f in t['fov']])
            log(f'head at {px:.2f} {py:.2f} {pz:.2f}: guest origin placed under it')
        self.seq += 1
        self.times[self.seq] = t['time_ns']
        self.times.pop(self.seq - 512, None)
        ox, oz = self.origin
        packet = struct.pack('<I7f', self.seq, px - ox, py + GUEST_FLOOR, pz - oz, qx, qy, qz, qw)
        for h, hand in enumerate(t['hands']):
            f, pressed, touched, trigger, grip, sx, sy = touch_controller(hand, h == 0, self.menu)
            (gx, gy, gz), q = imu_from_grip(hand['grip'][:3], hand['grip'][3:])
            packet += struct.pack('<3I7f4f', f, pressed, touched, gx - ox, gy + GUEST_FLOOR, gz - oz, *q, trigger, grip, sx, sy)
        self.guest.send(packet)

    def reader(self):
        try:
            while not self.stop:
                hdr = self.recv_exact(5)
                ptype, plen = hdr[0], struct.unpack('<I', hdr[1:])[0]
                if plen > 8 << 20:
                    raise ConnectionError('oversize packet')
                payload = self.recv_exact(plen) if plen else b''
                if ptype == 1:
                    self.hellos.put(json.loads(payload or b'{}'))
                elif ptype == 3:
                    self.on_tracking(payload)
                elif ptype == 7:
                    self.need_idr = True
                elif ptype == 9 and time.monotonic() - self.status_at > 5:
                    self.status_at = time.monotonic()
                    log('quest:', payload.decode('utf-8', 'replace')[:200])
        except (OSError, ValueError, ConnectionError) as e:
            if not self.stop:
                log('reader ended:', e)
        finally:
            self.stop = True

    def emit(self, pixels, when):
        """Encode one full-res padded frame and send it as VIDEO."""
        begin = time.monotonic() if self.profile else 0
        fw, fh = self.prof['eye_w'] * 2, self.prof['eye_h']
        frame = av.VideoFrame.from_numpy_buffer(pixels.view(np.uint8).reshape(ENC_H, CAP_W, 4), format='bgra')
        if (fw, fh) == (CAP_W, ENC_H):
            frame = self.reformatter.reformat(frame, format=self.encoder.pix_fmt)
        else:
            frame = self.reformatter.reformat(frame, width=fw, height=fh, format=self.encoder.pix_fmt)
        if self.gain < 0.995:   # Horizon's brightness slider: luma scaled, chroma pulled toward neutral
            luma, chroma = dim_tables(self.gain)
            for i, lut in enumerate((luma, chroma, chroma)):
                plane = np.frombuffer(frame.planes[i], np.uint8)
                np.take(lut, plane, out=plane)
        self.encoder_pts = max(self.encoder_pts + 1, int(time.monotonic() * 1_000_000))
        frame.pts = self.encoder_pts
        self.encoder_times[frame.pts] = when
        if len(self.encoder_times) > 512:
            raise ConnectionError('encoder frame queue exceeded bounded pose history')
        if self.need_idr:
            self.need_idr = False
            frame.pict_type = av.video.frame.PictureType.I
        converted = time.monotonic() if self.profile else 0
        packets = self.encoder.encode(frame)
        encoded = time.monotonic() if self.profile else 0
        for pkt in packets:
            captured_when = self.encoder_times.pop(pkt.pts, None)
            if captured_when is None:
                raise ConnectionError('encoded packet has no matching captured pose')
            header = struct.pack('<QQB', self.frame_id, captured_when, 1 if pkt.is_keyframe else 0)
            self.send(4, header + bytes(pkt))
            self.frame_id += 1
        if self.profile:
            sent = time.monotonic()
            self.profile_samples.append(((converted - begin) * 1000, (encoded - converted) * 1000, (sent - encoded) * 1000))
            if len(self.profile_samples) >= 120:
                samples = np.asarray(self.profile_samples)
                log('host frame ms (convert encode send), median/p95:',
                    np.round(np.median(samples, axis=0), 2).tolist(), np.round(np.percentile(samples, 95, axis=0), 2).tolist())
                self.profile_samples.clear()

    def audio(self):
        """The guest's sound to the headset: 10 ms PCM chunks from input/Audio.java, as VR4_AUDIO packets."""
        try:
            with socket.create_connection(('127.0.0.1', AUDIO_PORT), timeout=3) as a:
                a.settimeout(1)
                buf = b''
                while not self.stop:
                    try:
                        chunk = a.recv(AUDIO_CHUNK - len(buf))
                    except socket.timeout:
                        continue
                    if not chunk:
                        break
                    buf += chunk
                    if len(buf) == AUDIO_CHUNK:
                        self.send(6, struct.pack('<Q', time.time_ns()) + buf)
                        buf = b''
        except OSError as e:
            if not self.stop:
                log('guest audio unavailable:', e)

    def brightness(self):
        while not self.stop:
            self.gain = brightness_gain()
            time.sleep(2)

    def run(self):
        threading.Thread(target=self.brightness, daemon=True).start()
        reader = threading.Thread(target=self.reader, daemon=True)
        reader.start()
        try:
            self._run()
        finally:
            self.stop = True
            try:
                self.client.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass
            reader.join(timeout=3)

    def _run(self):
        last, sent_seq, sent_sig, sent_at, n, t0, skipped = None, None, None, 0.0, 0, time.monotonic(), {}
        last_when = 0
        last_pixels = None   # last encoded frame, re-sent while the display is frozen
        idle_interval = 1.0 / max(1, min(72, int(os.environ.get('EMUXR2_IDLE_FPS', '1'))))
        stamped_at = time.monotonic()
        while not self.stop:
            try:
                hello = self.hellos.get_nowait()
            except queue.Empty:
                pass
            else:
                self.configure(hello)
            now = time.monotonic()
            if self.encoder is not None and now - sent_at < 1.0 / self.prof['fps']:
                time.sleep(min(.001, 1.0 / self.prof['fps'] - (now - sent_at)))
                continue
            last, img = self.display.grab(last)
            # No stamped frames for 10 s: the compositor hung (acquiring a window buffer, say; the display then
            # freezes) or died. Restart the runtime; keepalive frames hold the headset connected meanwhile.
            if now - stamped_at > 10:
                log('no stamped frames for 10 s: restarting the guest VR runtime')
                restart_runtime()
                stamped_at = now + 30
            if img is None:
                # frozen display (idle scene): keep a frame a second flowing, or the
                # client times out and flaps through HELLO/CONFIG re-handshakes.
                # Prefer the last encoded frame; before the first one, the current
                # display contents with the (stale) stamp band blanked.
                if self.encoder is not None and now - sent_at >= idle_interval:
                    if last_pixels is None:
                        cur = self.display.current()
                        if cur is None:
                            time.sleep(0.001)
                            continue
                        hide_stamp(cur)
                        last_pixels = cur
                    self.emit(last_pixels, last_when)
                    sent_at, behind, n = now, 0, n + 1
                else:
                    time.sleep(0.001)
                    continue
            else:
                if self.encoder is None or img.shape != (CAP_H, CAP_W):
                    continue
                seq = pose_number(img)
                if seq is not None:
                    stamped_at = now
                when = self.times.get(seq)
                sig = img[::16, ::16]   # the emulator display repeats frames: skip ones identical to the last sent
                why = 'unstamped' if seq is None else 'unknown pose' if when is None else \
                    'repeat' if seq == sent_seq and np.array_equal(sig, sent_sig) else None
                if seq is not None and not self.times and idle_interval < 1.0:
                    # No XR tracking while the headset is asleep: decoder may still
                    # consume frames, but timestamp zero prevents incorrect display.
                    why, when = None, 0
                if why and now - sent_at > 1 and seq is not None:
                    # Preserve the captured pose on repeats; never label old pixels with a newer pose.
                    why, when = None, when or 0
                if why:
                    skipped[why] = skipped.get(why, 0) + 1
                    continue
                hide_stamp(img)
                self.emit(img, when)
                last_pixels = img
                last_when = when
                sent_seq, sent_sig, sent_at = seq, sig.copy(), now
                behind, n = (self.seq - seq if when else 0), n + 1
            if now - t0 >= 5:
                log(f'video: {n / (now - t0):.1f} fps @{self.prof["eye_w"] * 2}x{self.prof["eye_h"]}, '
                    f'pose {behind} behind, skipped {skipped} ')
                n, t0, skipped = 0, now, {}


def main():
    display = Display()
    log(f'emulator framebuffer {display.w}x{display.h}')
    guest = Guest()
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    try:
        start_guest()
        guest.connect()
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
            try:
                client.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)   # fails if it already reset
                client.settimeout(5)
                log('headset connected')
                Session(client, display, guest).run()
            except OSError as e:
                log('session ended:', e)
            finally:
                client.close()
            log('headset gone; waiting for it to reconnect')
    finally:
        srv.close()
        guest.close()
        try:
            adb('shell', 'for p in $(pgrep -f "[a]pp_process.*(Injector|Capture|Audio)"); do kill $p; done; '
                         'setprop ctl.start macvr-pose')
        except (OSError, subprocess.TimeoutExpired):
            pass


if __name__ == '__main__':
    try:
        main()
    except KeyboardInterrupt:
        log('streamer stopped')
