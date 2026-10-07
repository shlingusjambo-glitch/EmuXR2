#!/usr/bin/env python3
"""Horizon's display "distortion" mesh with no distortion: each eye's half of the display shows its eye's view straight.

Meta's runtime pre-warps each eye's image for the headset's lenses with a mesh: a grid of vertices spread evenly over
the eye's half of the display, each holding the view direction shown there (x and y tangent, once per colour channel,
40 bytes a vertex; a row holds both eyes). debug.oculus.distortionFileName names a replacement, and a .bin is read raw:
a 0x60-byte header (magic, device model, grid size, lens separation, display size, eye buffer size, each eye's field
of view in degrees: up, down, left, right), then the vertices. This one maps each eye's half linearly onto its field of
view, so the display carries plain perspective eye images the streamer hands to the headset as they are.
Usage: flat_mesh.py [out.bin]   (Quest 2 fields of view; no argument: self-check)
"""
import math, struct, sys

GRID = 32           # cells per side (the runtime's own capture mesh uses 32; a straight mapping needs no more)
MAGIC = 0x56347807
EMULATOR = 270      # the device model the runtime reports on the emulator (its own mesh header says so)
QUEST2_FOV = ((49, 45, 48, 50), (45, 49, 48, 50))   # per eye: left, right, up, down (degrees)


def mesh(fovs=QUEST2_FOV):
    """The mesh file for per-eye fields of view (left, right, up, down degrees)."""
    head = struct.pack('<6I2If4x2f4I', MAGIC, 0, EMULATOR, 0, EMULATOR, 0, GRID, GRID,
                       0.06485, 0.1203617, 0.063072, 3664, 1920, 1440, 1584)
    head += b''.join(struct.pack('<4f', u, d, l, r) for l, r, u, d in fovs)
    assert len(head) == 0x60
    tan = [[math.tan(math.radians(a)) for a in f] for f in fovs]
    out = [head]
    for j in range(GRID + 1):
        for l, r, u, d in tan:
            y = -d + (u + d) * j / GRID
            for i in range(GRID + 1):
                x = -l + (r + l) * i / GRID
                out.append(struct.pack('<6f16x', x, y, x, y, x, y))
    return b''.join(out)


def demo():
    m = mesh(((45, 45, 45, 45), (60, 60, 60, 60)))
    assert len(m) == 0x60 + (GRID + 1) * 2 * (GRID + 1) * 40
    assert struct.unpack_from('<I', m)[0] == MAGIC and struct.unpack_from('<2I', m, 0x18) == (GRID, GRID)
    x, y = struct.unpack_from('<2f', m, 0x60)                               # left eye, first vertex: bottom left
    assert abs(x + 1) < 1e-6 and abs(y + 1) < 1e-6
    x, y = struct.unpack_from('<2f', m, len(m) - 40)                        # right eye, last vertex: top right
    assert abs(x - math.sqrt(3)) < 1e-5 and abs(y - math.sqrt(3)) < 1e-5


if __name__ == '__main__':
    if len(sys.argv) == 1:
        demo()
        print('ok')
    else:
        open(sys.argv[1], 'wb').write(mesh())
