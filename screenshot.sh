#!/bin/sh
# Capture what the compositor scans out, upright: both eyes side by side (through bridge/stream.py's display
# capture, which works with the emulator's Vulkan composition). Usage: ./screenshot.sh [name] [directory]
set -eu
H=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
DEST=${2:-$H/screenshots}
NAME=${1:-$(date +%Y%m%d-%H%M%S)}
case "$NAME" in *[!a-zA-Z0-9_-]*) echo 'Use a simple screenshot name.' >&2; exit 1;; esac
mkdir -p "$DEST"
OUT="$DEST/$NAME.png"; RAW="$DEST/.$NAME.ppm"; rm -f "$OUT" "$RAW"
cd "$H/bridge" && .venv/bin/python - "$RAW" <<'EOF'
import sys, time, numpy as np
import stream
d = stream.Display(); time.sleep(0.3)
img = d.current()
if img is None: sys.exit('no frame')
rgb = np.stack([(img >> 16) & 255, (img >> 8) & 255, img & 255], -1).astype(np.uint8)
open(sys.argv[1], 'wb').write(b'P6 %d %d 255\n' % (rgb.shape[1], rgb.shape[0]) + rgb.tobytes())
EOF
sips -s format png "$RAW" --out "$OUT" >/dev/null && rm -f "$RAW"
echo "$OUT"
