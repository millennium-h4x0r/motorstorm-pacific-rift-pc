#!/usr/bin/env python3
"""Port hooks spliced into the lifted PPU code after scripts/lift.sh lifts it.

The lifted sources are generated from the user's executable and are never
committed, so changes to the game's own code are made here, as small
insertions that call a function defined in work/port. Each insertion is
matched against the exact text the lifter emits; if that text is not there
(a different lifter or executable), this script fails rather than guessing.
Running it again on an already patched tree changes nothing.

  work/tools/patch_lift.py [work/src/recomp]
"""
import glob, os, sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
SRC = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, 'work', 'src', 'recomp')

# (function, text after which the hook goes, hook text)
PATCHES = [
    # 60 fps mode. func_00435D18 is the frame timer's SetInterval(timer, n):
    # it stores n vblanks per frame at timer+0x18 and recomputes the fixed
    # step (n * 1e6 / 60 us). The game asks for 2 (30 fps); the vblank gate's
    # threshold is copied from the same field each frame. port_timer_interval
    # turns 2 into 1 when PS3RECOMP_FPS=60, which halves the step and lets a
    # frame through on every vblank.
    ('func_00435D18',
     'void func_00435D18(ppu_context* ctx) {\n    PPU_MM_SNAP\n',
     '    { void port_timer_interval(ppu_context*); port_timer_interval(ctx); } /* patch_lift.py */\n'),
    # func_008CF1C0 takes the frame step in f1 and hands it to the game clock
    # (func_0031BA20) and the other per-frame updates. port_frame_dt (60 fps
    # mode) scales it by the vblanks the previous frame really took, so a
    # frame that misses 60 does not run the game in slow motion.
    ('func_008CF1C0',
     'void func_008CF1C0(ppu_context* ctx) {\n    PPU_MM_SNAP\n',
     '    { void port_frame_dt(ppu_context*); port_frame_dt(ctx); } /* patch_lift.py */\n'),
]


def main():
    files = sorted(glob.glob(os.path.join(SRC, 'ppu_recomp_*.cpp')))
    if not files:
        sys.exit(f'patch_lift: no lifted sources in {SRC}')
    for fn, anchor, hook in PATCHES:
        hits = [p for p in files if anchor in open(p, encoding='utf-8', errors='surrogateescape').read()]
        if len(hits) != 1:
            sys.exit(f'patch_lift: {fn}: expected its lifted entry once, found it in {len(hits)} files')
        p = hits[0]
        s = open(p, encoding='utf-8', errors='surrogateescape').read()
        if anchor + hook in s:
            print(f'patch_lift: {fn}: already patched ({os.path.basename(p)})')
            continue
        if s.count(anchor) != 1:
            sys.exit(f'patch_lift: {fn}: entry appears {s.count(anchor)} times in {p}')
        open(p, 'w', encoding='utf-8', errors='surrogateescape').write(s.replace(anchor, anchor + hook))
        print(f'patch_lift: {fn}: patched ({os.path.basename(p)})')


if __name__ == '__main__':
    main()
