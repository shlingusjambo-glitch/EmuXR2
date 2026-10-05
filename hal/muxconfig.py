"""Turn the firmware's /vendor/etc/cameramuxmode/configv2.json into the line format macvr-hal reads:
  rate <variant>
  slot <cameraId>...                      (one line per slot of the mode's configuration)
  mode <name> <configuration>
  illum <name>...
  purpose <name> <slot>...
  control <name> <slot>...
usage: muxconfig.py <configv2.json> > muxmode.txt"""
import json, sys
c = json.load(open(sys.argv[1]))
wt = next(iter(c['cameraSystems'].values()))
m = wt['modes'][0]; cfg = wt['configurations'][m['configuration']]
print('rate', cfg.get('frameRateVariant', 'utility'), cfg.get('frameRateMultiplier', 1.0))
for s in cfg['slots']: print('slot', *s['cameraIds'])
print('mode', m['name'], m['configuration'])
print('illum', *m.get('illuminations', []))
for p in m.get('purposes', []): print('purpose', p['name'], *p['slots'])
for p in m.get('controlPurposes', []): print('control', p['name'], *p['slots'])
