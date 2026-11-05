#!/usr/bin/env python3
# sym.py <pid> <samples> : leaf and inclusive function histograms
import sys, subprocess, bisect, collections
pid, sf = sys.argv[1], sys.argv[2]
maps = []
for l in open('/proc/%s/maps' % pid):
    a, perm, off, dev, ino, *path = l.split()
    lo, hi = [int(x, 16) for x in a.split('-')]
    maps.append((lo, hi, int(off, 16), path[0] if path else '?', perm))
exe = open('/proc/%s/cmdline' % pid).read().split('\0')[0]
syms = []
for l in subprocess.run(['nm', '-C', '--defined-only', exe], capture_output=True, text=True).stdout.splitlines():
    p = l.split(' ', 2)
    if len(p) == 3 and p[1] in 'tTwW': syms.append((int(p[0], 16), p[2]))
syms.sort(); addrs = [s[0] for s in syms]
def name(ip):
    for lo, hi, off, path, perm in maps:
        if lo <= ip < hi:
            if path == exe:
                # PIE: file offset mapping
                va = ip - lo + off
                # symbols are link addresses; text maps at off== section offset, assume va==link for PIE base 0
                i = bisect.bisect_right(addrs, va) - 1
                return syms[i][1].split('(')[0] if i >= 0 else '?exe'
            return path.split('/')[-1]
    return '?'
leaf = collections.Counter(); incl = collections.Counter(); n = 0
cache = {}
def nm_(ip):
    if ip not in cache: cache[ip] = name(ip)
    return cache[ip]
for l in open(sf):
    ips = [int(x, 16) for x in l.split()]
    if not ips: continue
    n += 1
    leaf[nm_(ips[0])] += 1
    for f in set(nm_(x) for x in ips): incl[f] += 1
print("samples", n)
print("--- leaf"); [print("%5.1f%% %s" % (100*c/n, f)) for f, c in leaf.most_common(100000)]
print("--- inclusive (where callchains exist)"); [print("%5.1f%% %s" % (100*c/n, f)) for f, c in incl.most_common(40)]
