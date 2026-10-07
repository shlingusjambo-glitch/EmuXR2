#!/usr/bin/env python3
"""Meta config parameters an app needs on an offline emulator, seeded into its device-config cache.

Meta's apps read their config parameters (gatekeepers, feature switches) from the MobileConfig service in the Horizon
app, which gets them from Meta's servers. Offline they all fall back to built-in defaults, and some defaults break
things a headset has switched on. Each app's client caches values in shared_prefs/DeviceConfigCache.xml (a JSON list
under "Cache") and starts from it; this adds or replaces entries there and restarts the app.
Usage: meta_params.py   (emulator running; safe to repeat)
"""
import html, json, os, re, subprocess

ADB = [os.environ.get('ANDROID_SDK_ROOT', os.path.expanduser('~/Library/Android/sdk')) + '/platform-tools/adb',
       '-s', os.environ.get('EMUXR2_SERIAL', 'emulator-5554')]
PARAMS = {
    # the Library keeps the filter you pick (Unknown Sources...): without it, choosing one saves it as the default,
    # which reloads the dropdowns from the built-in default (All) at once
    'com.oculus.systemux': {'oculus_systemux:oculus_sysux_library_filter_view': True},
}


def shell(cmd):
    return subprocess.run(ADB + ['shell', cmd], capture_output=True, text=True, timeout=60).stdout


def seeded(xml, params):
    """The cache file with these parameters set (a new file when there is none)."""
    m = re.search(r'<string name="Cache">(.*?)</string>', xml or '', re.S)
    cache = json.loads(html.unescape(m.group(1))) if m else {'values': [], 'version': '1.0'}
    values = [v for v in cache['values'] if v['param_name'] not in params]
    for name, value in params.items():
        kind = 'boolean' if isinstance(value, bool) else 'long' if isinstance(value, int) else 'string'
        values.append({'param_name': name, 'value': str(value).lower() if kind == 'boolean' else str(value),
                       'type': kind, 'sessionless': True, 'server_eval_timestamp': 1})
    cache['values'] = values
    body = html.escape(json.dumps(cache, separators=(',', ':')), quote=True)
    return ("<?xml version='1.0' encoding='utf-8' standalone='yes' ?>\n<map>\n"
            f'    <string name="Cache">{body}</string>\n</map>\n')


def main():
    for pkg, params in PARAMS.items():
        path = f'/data/data/{pkg}/shared_prefs/DeviceConfigCache.xml'
        new = seeded(shell(f'cat {path} 2>/dev/null'), params)
        shell(f'am force-stop {pkg}')
        owner = shell(f'stat -c %u:%g /data/data/{pkg}').strip()
        subprocess.run(ADB + ['shell', f'cat > {path}; chown {owner} {path}; chmod 660 {path}; restorecon {path}'],
                       input=new, text=True, timeout=60)
        print(f'{pkg}: {", ".join(params)}')


def demo():
    xml = seeded(None, {'a:b': True})
    assert 'a:b' in xml and '&quot;true&quot;' in xml
    again = seeded(xml, {'a:b': False, 'c:d': 'x'})
    cache = json.loads(html.unescape(re.search(r'"Cache">(.*?)</string>', again, re.S).group(1)))
    assert [(v['param_name'], v['value']) for v in cache['values']] == [('a:b', 'false'), ('c:d', 'x')]


if __name__ == '__main__':
    import sys
    if sys.argv[1:] == ['test']:
        demo(); print('ok')
    else:
        main()
