"""Print files whose contents differ between two ext4 images (dumps each tree once to a temp dir)."""
import hashlib, os, shutil, subprocess, sys, tempfile
D = '/opt/homebrew/opt/e2fsprogs/sbin/debugfs'
def sums(img):
    t = tempfile.mkdtemp(dir=os.path.expanduser('~/MacVRFirmware'))
    subprocess.run([D, '-R', f'rdump / {t}', img], capture_output=True)
    out = {}
    for root, _, files in os.walk(t):
        for f in files:
            p = os.path.join(root, f)
            if os.path.islink(p): continue
            h = hashlib.sha1()
            with open(p, 'rb') as fh:
                for b in iter(lambda: fh.read(1 << 20), b''): h.update(b)
            out[p[len(t):]] = h.hexdigest()
    shutil.rmtree(t); return out
a, b = sums(sys.argv[1]), sums(sys.argv[2])
for k in sorted(a):
    if b.get(k) != a[k]: print(k)
