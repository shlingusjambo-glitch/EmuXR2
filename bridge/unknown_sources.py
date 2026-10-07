#!/usr/bin/env python3
"""Sideloaded apps in Horizon's App Library ("Unknown Sources"), without a Meta account.

The Library lists every installed app that isn't in Meta's content service (OCMS) library as an unknown source, but it
asks OCMS for that library first; with no account OCMS throws "Invalid credentials or user id", and the whole Unknown
Sources scan comes back empty. In Meta's own kiosk mode (Quest for Business) OCMS serves the library of a local
"kiosk_user" instead, so the scan works and sideloaded games show up and launch. Kiosk mode is the preference
q4b_kiosk_enabled up to v54; later versions (v64) read KIOSK_ENABLED from OCMS's app restrictions instead, which Android
keeps in /data/system/users/0/res_<package>.xml.
Horizon's environment packages are installed beside the games; this also adds them to that local library (category
Environments, as on a Quest), so they aren't listed as unknown sources.
Usage: unknown_sources.py   (emulator running; safe to repeat)
"""
import os, re, sqlite3, subprocess, tempfile

ADB = [os.environ.get('ANDROID_SDK_ROOT', os.path.expanduser('~/Library/Android/sdk')) + '/platform-tools/adb',
       '-s', os.environ.get('EMUXR2_SERIAL', 'emulator-5554')]
DB = '/data/data/com.oculus.ocms/databases/library_database'
USER = 'kiosk_user'
ENVIRONMENTS = ('com.meta.environment.', 'com.oculus.environment.')


# the entry columns written for each environment (those this Library's schema has)
ENTRY = ('latest_version_code', 'status', 'category', 'update_type')


# OCMS's app restrictions (Android's UserManager reads the file on every request)
RESTRICTIONS = '/data/system/users/0/res_com.oculus.ocms.xml'
KIOSK_XML = ("<?xml version='1.0' encoding='utf-8' standalone='yes' ?>\n"
             '<restrictions>\n<entry key="KIOSK_ENABLED" type="b">true</entry>\n</restrictions>\n')


def shell(cmd):
    return subprocess.run(ADB + ['shell', cmd], capture_output=True, text=True, timeout=60).stdout


def environment_row(pkg):
    dump = shell(f'dumpsys package {pkg}')
    code = re.search(r'versionCode=(\d+)', dump)
    name = re.search(r'versionName=(\S+)', dump)
    title = pkg.rsplit('.', 1)[-1].replace('home', ' home').replace('station', ' station').title()
    return {
        'user_id': USER, 'item_id': pkg, 'name': title, 'package_name': pkg,
        'latest_version_code': int(code.group(1)) if code else 0, 'latest_version_name': name.group(1) if name else '',
        'status': 'INSTALLED', 'category': 'ENVIRONMENTS', 'platform': 'ANDROID_6DOF', 'grant_reason': 'UNKNOWN',
        'cloud_storage_status': 'DISABLED', 'comfort_rating': 'NOT_RATED', 'microphone_usage': 'NONE',
        'report_method': 'UNSET', 'app_2d_mode': 'NONE', 'head_tracking': 'ALLOW_6DOF', 'livestreaming_status': 'UNKNOWN',
        'play_modes': 'STANDING', 'user_interaction_modes': 'SINGLE_USER', 'entitlement_hash': 'local',
        'update_type': 'UNSET',   # newer Libraries (v64) only; VrShell crashes on an empty one
    }


def columns(local):
    """The library table's columns and types (this Horizon version's schema)."""
    db = sqlite3.connect(local)
    try:
        return {c: t for _, c, t, *_ in db.execute('pragma table_info(library)')}
    finally:
        db.close()


def listed(local, keys):
    """The kiosk user's rows the local library already holds, as {package: (values of keys)}."""
    db = sqlite3.connect(local)
    try:
        return {r[0]: r[1:] for r in db.execute(f'select item_id, {", ".join(keys)} from library where user_id = ?', (USER,))}
    except sqlite3.Error:
        return None
    finally:
        db.close()


def kiosk_enabled():
    return ('true' in shell('oculuspreferences --getc q4b_kiosk_enabled')
            or 'KIOSK_ENABLED" type="b">true' in shell(f'cat {RESTRICTIONS} 2>/dev/null'))


def enable_kiosk():
    if 'true' in shell('oculuspreferences --setc q4b_kiosk_enabled true; oculuspreferences --getc q4b_kiosk_enabled'):
        return
    with tempfile.NamedTemporaryFile('w', suffix='.xml') as f:   # the preference is gone: app restrictions
        f.write(KIOSK_XML); f.flush()
        subprocess.run(ADB + ['push', f.name, '/data/local/tmp/res.xml'], capture_output=True, timeout=60)
    shell(f'cat /data/local/tmp/res.xml > {RESTRICTIONS}; rm /data/local/tmp/res.xml; '
          f'chown system:system {RESTRICTIONS}; chmod 600 {RESTRICTIONS}; restorecon {RESTRICTIONS}')


def main():
    kiosk = kiosk_enabled()
    envs = [l.split(':', 1)[1].strip() for l in shell('pm list packages -3').splitlines()
            if l.startswith('package:') and l.split(':', 1)[1].strip().startswith(ENVIRONMENTS)]
    rows = {pkg: environment_row(pkg) for pkg in envs}
    with tempfile.TemporaryDirectory() as d:
        local = os.path.join(d, 'library_database')
        subprocess.run(ADB + ['shell', f'cat {DB}'], stdout=open(local, 'wb'), timeout=60)
        # Already set up: leave OCMS and SystemUX running (restarting SystemUX closes every open panel)
        cols = columns(local)
        keys = sorted(k for k in ENTRY if k in cols)
        if kiosk and listed(local, keys) == {p: tuple(r[k] for k in keys) for p, r in rows.items()}:
            print('local library: up to date; sideloaded apps list under Unknown Sources')
            return
    enable_kiosk()
    shell('am force-stop com.oculus.ocms')
    with tempfile.TemporaryDirectory() as d:
        local = os.path.join(d, 'library_database')
        subprocess.run(ADB + ['shell', f'cat {DB}'], stdout=open(local, 'wb'), timeout=60)
        db = sqlite3.connect(local)
        # every column set: OCMS's row reader rejects nulls (enum names, lists it splits, the entitlement hash)
        blank = {c: '' if t == 'TEXT' else 0 for _, c, t, *_ in db.execute('pragma table_info(library)')}
        db.execute('delete from library where user_id = ?', (USER,))
        for pkg in envs:
            r = dict(blank, **{k: v for k, v in rows[pkg].items() if k in blank})   # only this schema's columns
            db.execute(f'insert into library ({", ".join(r)}) values ({", ".join("?" * len(r))})', list(r.values()))
        db.commit()
        db.close()
        subprocess.run(ADB + ['push', local, '/data/local/tmp/library_database'], capture_output=True, timeout=60)
    # written in place: the file keeps OCMS's owner and SELinux label
    shell(f'cat /data/local/tmp/library_database > {DB}; rm /data/local/tmp/library_database; '
          'am force-stop com.oculus.ocms; am force-stop com.oculus.systemux')
    print('local library: kiosk user with', len(envs), 'environments; sideloaded apps list under Unknown Sources')


if __name__ == '__main__':
    main()
