#!/usr/bin/env python3
"""Check a Quest OTA's extracted partitions against what EmuXR2 assumes (read-only: nothing is modified).

EmuXR2 was built against Horizon v54 (Android 12, API 32). A newer OTA can change the Android base, Meta's vendor HAL
interfaces, the runtime's debug properties and the Library service; this reports each assumption as ok, changed
(with what to look at) or missing, so a port starts from a list instead of a failed boot. See docs/porting.md.
Usage: ota_check.py <dir with img/system.img, system_ext.img, product.img, vendor.img>
"""
import os, re, subprocess, sys, tempfile, zipfile

DEBUGFS = '/opt/homebrew/opt/e2fsprogs/sbin/debugfs'
HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
SDK = os.environ.get('ANDROID_SDK_ROOT', os.path.expanduser('~/Library/Android/sdk'))
results = []


def report(name, ok, detail=''):
    results.append((('ok' if ok else 'CHANGED') if ok is not None else 'MISSING', name, detail))


def cat(img, path):
    r = subprocess.run([DEBUGFS, '-R', f'cat {path}', img], capture_output=True)
    return r.stdout if r.returncode == 0 and r.stdout else None


def ls(img, path):
    r = subprocess.run([DEBUGFS, '-R', f'ls -p {path}', img], capture_output=True, text=True)
    return [l.split('/')[5] for l in r.stdout.splitlines() if l.count('/') >= 6 and l.split('/')[5] not in ('.', '..')]


def strings(data, minimum=6):
    return set(m.decode() for m in re.findall(rb'[\x20-\x7e]{%d,}' % minimum, data))


def apk_strings(img, path, member_pattern):
    data = cat(img, path)
    if not data:
        return None
    with tempfile.NamedTemporaryFile(suffix='.apk') as f:
        f.write(data); f.flush()
        z = zipfile.ZipFile(f.name)
        found = set()
        for n in z.namelist():
            if re.search(member_pattern, n):
                found |= strings(z.read(n))
        return found


def main():
    d = sys.argv[1]
    img = {p: os.path.join(d, 'img', p + '.img') for p in ('system', 'system_ext', 'product', 'vendor')}
    for p, f in img.items():
        if not os.path.exists(f):
            report(f'{p}.img', None, f'not found in {d}/img')

    # Android base: the emulator system image the Quest partitions boot on must match its API level
    prop = (cat(img['system'], '/system/build.prop') or b'').decode(errors='replace')
    get = lambda k: (re.search(rf'^{re.escape(k)}=(.*)$', prop, re.M) or [None, '?'])[1]
    sdk, release = get('ro.build.version.sdk'), get('ro.build.version.release')
    report('Android base', sdk == '32', f'Android {release} (API {sdk}), build {get("ro.build.version.incremental")}; '
           'EmuXR2 boots API 32 (patch.sh offsets into the API 32 emulator system.img)' if sdk != '32' else f'Android {release}, API 32')
    image = os.path.join(SDK, 'system-images', f'android-{sdk}', 'google_apis', 'arm64-v8a', 'system.img')
    report('matching emulator image', True if os.path.exists(image) else None, image)

    # Meta's vendor HAL interfaces that hal/ implements (names and versions from our manifest)
    manifest = open(os.path.join(ROOT, 'hal', 'macvr-hal.xml')).read()
    ours = re.findall(r'<name>(vendor\.oculus\.[\w.]+)</name>\s*<transport>\w+</transport>\s*<version>([\d.]+)</version>', manifest)
    libs = set(ls(img['system'], '/system/lib64')) | set(ls(img['system_ext'], '/lib64')) | set(ls(img['vendor'], '/lib64'))
    ver = lambda l: tuple(int(x) for x in re.search(r'@([\d.]+)\.so$', l).group(1).split('.'))
    for name, version in ours:
        lib = f'{name}@{version}.so'
        newer = sorted(l for l in libs if l.startswith(name + '@') and ver(l) > tuple(map(int, version.split('.'))))
        report(f'HAL {name}@{version}', lib in libs,
               ('also ' + ', '.join(newer) + ': regenerate signatures (hidlsig.py, vtable.py) and extend hal/') if newer else
               ('' if lib in libs else 'interface library gone: served by input/DeviceCert.java instead (check its method layout, read from a client)'))

    # the VR runtime: the properties and symbols EmuXR2 relies on
    rt = apk_strings(img['system'], '/system/priv-app/VrDriver/VrDriver.apk', r'arm64-v8a/libvrruntimeservice\.so$')
    if rt is None:
        report('VR runtime (VrDriver)', None, '/system/priv-app/VrDriver/VrDriver.apk')
    else:
        for s, why in (('debug.oculus.distortionFileName', 'flat display mesh (bridge/flat_mesh.py, vendor.prop)'),
                       ('AImageReader_newWithUsage', 'swapchain size clamp hook (egl/egl_macvr.c)'),
                       ('debug.oculus.eyeFovUp', 'eye field-of-view properties'),
                       ('debug.oculus.refreshRate', 'refresh rate property')):
            report(f'runtime: {s}', s in rt, '' if s in rt else f'used by {why}')
        report('runtime: frosted glass', True,
               'present in this runtime: panels need oculus_xrruntime:oculus_frosted_glass (see Prism notes)'
               if any('glass' in x.lower() for x in rt) else 'not in this runtime (as in v54)')

    # Library: Unknown Sources through OCMS's local kiosk library (bridge/unknown_sources.py)
    ocms = apk_strings(img['system'], '/system/priv-app/OCMS/OCMS.apk', r'classes\d*\.dex$')
    if ocms is None:
        report('OCMS (Library service)', None, '/system/priv-app/OCMS/OCMS.apk')
    else:
        for s in ('q4b_kiosk_enabled', 'kiosk_user', 'library_database'):
            report(f'OCMS: {s}', s in ocms, '' if s in ocms else 'bridge/unknown_sources.py needs another route')

    # tracking: poses enter through Meta's tracking injection service (input/Injector.java)
    report('tracking injection service', 'libtrackinginjection-service.so' in libs, 'libtrackinginjection-service.so')

    # files patch.sh edits in the Quest partitions
    for p, path in (('system', '/system/etc/vintf/compatibility_matrix.device.xml'),
                    ('system', '/system/etc/selinux/plat_sepolicy.cil'),
                    ('system_ext', '/etc/selinux/system_ext_sepolicy.cil'),
                    ('system', '/system/build.prop')):
        report(f'patched file {path}', True if cat(img[p], path) else None, p)

    width = max(len(n) for _, n, _ in results)
    for status, name, detail in results:
        print(f'{status:8} {name:{width}}  {detail}')
    return 1 if any(s != 'ok' for s, _, _ in results) else 0


if __name__ == '__main__':
    sys.exit(main())
