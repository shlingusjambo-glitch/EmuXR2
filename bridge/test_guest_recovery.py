"""A failed guest socket must not stall headset packets or replay a pose backlog."""
import struct
import threading
import time
import stream

failed = threading.Event()
recovered = threading.Event()
received = []

class Broken:
    def sendall(self, data):
        failed.set()
        raise OSError('injected guest disconnect')
    def close(self): pass

class Reconnected:
    def sendall(self, data):
        received.append(struct.unpack('<I', data)[0])
        recovered.set()
    def shutdown(self, how): pass
    def close(self): pass

g = stream.Guest()
g.sock = Broken()
g.connect = lambda: setattr(g, 'sock', Reconnected())
g.worker = threading.Thread(target=g._pump)
g.worker.start()
g.send(struct.pack('<I', 0))
assert failed.wait(2)
start = time.monotonic()
for seq in range(1, 1001):
    g.send(struct.pack('<I', seq))
assert time.monotonic() - start < .1, 'headset reader blocked on guest recovery'
assert recovered.wait(3)
g.close()
assert not g.worker.is_alive()
assert received == [1000], f'queued stale poses were replayed: {received}'
print('PASS: guest reconnect keeps only latest pose, packet reader stays responsive, worker stops')
