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
import os, re
libsyms = {}
def lib_table(path):
    """Symbols of a shared library: its own .symtab, else the debug file its
    build id names under /usr/lib/debug (libc6-dbg), else its dynamic table.
    Returned with the PT_LOAD segments, to turn file offsets into addresses."""
    if path in libsyms: return libsyms[path]
    src = path
    try:
        bid = re.search(r'Build ID: ([0-9a-f]+)', subprocess.run(['readelf', '-n', path],
                        capture_output=True, text=True).stdout)
        if bid:
            dbg = '/usr/lib/debug/.build-id/%s/%s.debug' % (bid.group(1)[:2], bid.group(1)[2:])
            if os.path.exists(dbg): src = dbg
    except Exception: pass
    tab = []
    for args in (['nm', '-C', '--defined-only', src], ['nm', '-C', '-D', '--defined-only', path]):
        for l in subprocess.run(args, capture_output=True, text=True).stdout.splitlines():
            q = l.split(' ', 2)
            if len(q) == 3 and q[1] in 'tTwWiI':
                try: tab.append((int(q[0], 16), q[2]))
                except ValueError: pass
        if tab: break
    tab.sort()
    loads = []
    for l in subprocess.run(['readelf', '-lW', path], capture_output=True, text=True).stdout.splitlines():
        f = l.split()
        if f and f[0] == 'LOAD':
            loads.append((int(f[1], 16), int(f[2], 16), int(f[4], 16)))   # offset, vaddr, filesz
    libsyms[path] = (tab, [t[0] for t in tab], loads)
    return libsyms[path]
def name(ip):
    for lo, hi, off, path, perm in maps:
        if lo <= ip < hi:
            if path == exe:
                # PIE: file offset mapping
                va = ip - lo + off
                # symbols are link addresses; text maps at off== section offset, assume va==link for PIE base 0
                i = bisect.bisect_right(addrs, va) - 1
                return syms[i][1].split('(')[0] if i >= 0 else '?exe'
            short = path.split('/')[-1]
            if path.startswith('/') and os.path.exists(path):
                tab, ad, loads = lib_table(path)
                fo = ip - lo + off
                for o, v, n in loads:
                    if o <= fo < o + n:
                        i = bisect.bisect_right(ad, fo - o + v) - 1
                        if i >= 0: return short + ':' + tab[i][1].split('(')[0]
                        break
            return short
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
