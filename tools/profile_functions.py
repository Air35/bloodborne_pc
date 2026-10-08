#!/usr/bin/env python3
"""sotc: per-function self time (leaf samples) of a SOTC_PROFILE_OUT dump: profile_functions.py dump [exe]"""
import collections, re, subprocess, sys
exe = sys.argv[2] if len(sys.argv) > 2 else 'out/gpu/bb-probe.exe'
rows, samples = [], 1
for line in open(sys.argv[1], encoding='utf-8', errors='replace'):
    parts = line.split()
    if parts[0] == 'samples':
        samples = int(parts[1]); continue
    rows.append((' '.join(parts[:-2]), int(parts[-2]), int(parts[-1])))
own = [r for r in rows if r[0].startswith('bb-probe.exe+')]
addrs = [hex(0x140000000 + int(r[0].split('+')[1], 16)) for r in own]
names = []
for i in range(0, len(addrs), 2000):
    out = subprocess.run(['llvm-symbolizer', f'--obj={exe}', '--functions=short', '--inlines=false' if False else '--no-inlines'] + addrs[i:i+2000],
                         capture_output=True, text=True).stdout.strip().split('\n\n')
    names += [o.split('\n')[0] for o in out]
self_time = collections.Counter()
for (where, leaf, inc), name in zip(own, names):
    self_time[name] += leaf
other = collections.Counter()
for where, leaf, inc in rows:
    if not where.startswith('bb-probe.exe+'):
        other[re.sub(r'\+0x[0-9a-f]+$', '', where)] += leaf
print(f'{samples} samples; self time by function:')
for name, n in self_time.most_common(40):
    print(f'  {100*n/samples:5.1f}%  {name[:110]}')
print('outside the loader (by module):')
for name, n in other.most_common(10):
    print(f'  {100*n/samples:5.1f}%  {name}')
