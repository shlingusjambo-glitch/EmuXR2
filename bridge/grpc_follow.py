#!/usr/bin/env python3
"""Receive the emulator's gRPC display stream into a memory-mapped frame file, counting frames in <file>.seq.

Run by stream.GrpcDisplay as its own process: inside the busy streamer process, the receiving thread waited on
Python's lock behind conversion and encoding, gRPC held the emulator's display posting back meanwhile, and the
guest's frame rate fell with the stream's. Exits when its parent closes stdin.
Usage: grpc_follow.py <port> <frame file>   (EMUXR2_GRPC_TOKEN: the console token, when the emulator wants one)
"""
import mmap, os, struct, sys, threading, time

os.environ.setdefault('GRPC_VERBOSITY', 'ERROR')
import grpc

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'input', 'out', 'grpc'))
import emulator_controller_pb2 as pb, emulator_controller_pb2_grpc as rpc


def main():
    port, path = int(sys.argv[1]), sys.argv[2]
    token = os.environ.get('EMUXR2_GRPC_TOKEN')
    auth = [('authorization', 'Bearer ' + token)] if token else None
    with open(path + '.seq', 'r+b') as f:
        seq = mmap.mmap(f.fileno(), 8)
    threading.Thread(target=lambda: (sys.stdin.read(), os._exit(0)), daemon=True).start()
    stub = rpc.EmulatorControllerStub(grpc.insecure_channel(f'localhost:{port}'))
    fmt = pb.ImageFormat(format=pb.ImageFormat.RGBA8888, transport=pb.ImageTransport(
        channel=pb.ImageTransport.MMAP, handle='file://' + path))
    n = 0
    while True:
        try:
            for _ in stub.streamScreenshot(fmt, metadata=auth):
                n += 1
                seq[:8] = struct.pack('<Q', n)
        except grpc.RpcError as e:   # emulator restarting: keep trying
            print('gRPC display stream ended:', e.code(), file=sys.stderr, flush=True)
            time.sleep(1)


if __name__ == '__main__':
    main()
