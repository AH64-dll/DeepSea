#!/usr/bin/env python3
"""mg_symbolize.py — attribute mg_sampler samples to code regions/symbols.

Usage:
  mg_symbolize.py SAMPLES MAPS [BIN ...]

SAMPLES: lines "tid:rip_hex" from mg_sampler.so
MAPS:    /proc/<pid>/maps snapshot taken while the process was alive
BIN:     unstripped binaries/.so to symbolize against (runner, module)

Tier classification:
  recomp:*     RIP inside a gGZLE01_recomp*.so region (native generated code)
  jit:anon     RIP inside anonymous executable memory (fallback JIT blocks)
  runner:*     RIP inside moderngekko-run (runtime: dispatch/interp/mem/GX)
  lib:<name>   RIP inside another mapped file (libc, libstdc++, ld, ...)
"""
import bisect
import subprocess
import sys
from collections import Counter


def load_maps(path):
    regions = []
    with open(path) as f:
        for line in f:
            parts = line.split()
            if len(parts) < 6 or 'x' not in parts[1]:
                continue
            try:
                lo, hi = (int(x, 16) for x in parts[0].split('-'))
                off = int(parts[2], 16)
            except ValueError:
                continue
            name = ' '.join(parts[5:]) if len(parts) > 5 else ''
            regions.append((lo, hi, off, name))
    regions.sort()
    return regions


class Symtab:
    def __init__(self, path):
        self.names = []
        self.addrs = []
        self.pie = True
        try:
            hdr = subprocess.run(['readelf', '-h', path],
                                 capture_output=True, text=True).stdout
            self.pie = 'DYN' in hdr.split('Type:')[1].split('\n')[0]
            out = subprocess.run(['nm', '-n', '--defined-only', path],
                                 capture_output=True, text=True).stdout
        except (OSError, IndexError):
            return
        for line in out.splitlines():
            p = line.split(None, 2)
            if len(p) == 3 and all(c in '0123456789abcdefABCDEF' for c in p[0]):
                self.addrs.append(int(p[0], 16))
                self.names.append(p[2])

    def lookup(self, key):
        i = bisect.bisect_right(self.addrs, key) - 1
        return self.names[i] if i >= 0 else '?'


def main():
    if len(sys.argv) < 3:
        sys.exit('usage: mg_symbolize.py SAMPLES MAPS [--from MS] [--to MS] [BIN ...]')
    samples_path, maps_path = sys.argv[1], sys.argv[2]
    t_from, t_to = 0, 1 << 62
    bins = {}
    args = sys.argv[3:]
    i = 0
    while i < len(args):
        if args[i] in ('--from', '--to'):
            if i + 1 >= len(args):
                sys.exit(f'{args[i]} requires a value')
            try:
                value = int(args[i + 1])
            except ValueError:
                sys.exit(f'{args[i]} requires an integer: {args[i + 1]}')
            if args[i] == '--from':
                t_from = value
            else:
                t_to = value
            i += 2
        else:
            bins[args[i]] = Symtab(args[i]); i += 1
    # match maps entries by realpath suffix too
    def bin_for(name):
        if name in bins:
            return bins[name]
        for k, v in bins.items():
            if name.endswith(k.split('/')[-1]):
                return v
        return None

    regions = load_maps(maps_path)
    lo_list = [r[0] for r in regions]

    def find(ip):
        i = bisect.bisect_right(lo_list, ip) - 1
        while i >= 0:
            lo, hi, off, name = regions[i]
            if lo <= ip < hi:
                return lo, off, name
            i -= 1
        return None, None, ''

    per_sym = Counter()
    per_tid_region = Counter()
    total = 0
    t0 = None
    with open(samples_path) as f:
        for line in f:
            line = line.strip()
            if ':' not in line:
                continue
            head, ip_s = line.rsplit(':', 1)
            try:
                if '@' in head:
                    ms_s, tid_s = head.split('@', 1)
                    ms = int(ms_s)
                else:
                    ms, tid_s = 0, head
                ip = int(ip_s, 16)
            except ValueError:
                continue
            if t0 is None:
                t0 = ms
            rel = ms - t0
            if rel < t_from or rel >= t_to:
                continue
            total += 1
            rlo, roff, rname = find(ip)
            base = rname.split('/')[-1] if rname else ''
            sym = None
            if rname:
                st = bin_for(rname)
                if st is not None:
                    # PIE/.so: symbol key = ip - region_lo + p_offset
                    # non-PIE exec: symbol key = ip
                    sym = st.lookup(ip if not st.pie else ip - rlo + roff)
            if base.startswith('gGZLE01_recomp'):
                key = f'recomp:{sym or "?"}'
            elif 'moderngekko' in base:
                key = f'runner:{sym or "?"}'
            elif rname:
                key = f'lib:{base}'
            else:
                key = 'jit:anon'
            per_sym[key] += 1
            per_tid_region[(tid_s, key.split(':')[0])] += 1

    print(f'total_samples={total}')
    if total == 0:
        print('(no samples in range)')
        return
    print('== tier rollup ==')
    roll = Counter()
    for k, v in per_sym.items():
        roll[k.split(':')[0]] += v
    for k, v in roll.most_common():
        print(f'{k:10s} {v:8d}  {100.0*v/total:5.1f}%')
    print('== top symbols ==')
    for k, v in per_sym.most_common(60):
        print(f'{v:8d}  {100.0*v/total:5.1f}%  {k}')
    print('== per-thread tier share ==')
    tid_tot = Counter()
    for (t, tier), v in per_tid_region.items():
        tid_tot[t] += v
    for t in sorted(tid_tot, key=lambda x: -tid_tot[x]):
        parts = ', '.join(f'{tier}={100.0*v/tid_tot[t]:.0f}%'
                          for (tt, tier), v in
                          sorted(per_tid_region.items(), key=lambda kv: -kv[1])
                          if tt == t)
        print(f'tid {t}: n={tid_tot[t]}  {parts}')


if __name__ == '__main__':
    main()
