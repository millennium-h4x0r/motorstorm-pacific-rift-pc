#!/usr/bin/env python3
"""Lift MultiStream DSP plugins at the local-store bases the core loads them to.

The MultiStream SPU core (a lifted taskset task) copies each DSP plugin
(`mstream_dsp_*.pic`, position-independent, linked at 0x80) from a heap buffer
into local store at an address it allocates itself, and calls it there. A lift
has absolute addresses, so it is only valid at one base; the runtime finds it
by address (spu_overlay_register_pinned) and checks the code bytes before it
dispatches. A plugin loaded anywhere else, or not listed here, runs on the
interpreter -- correct, but about 5x too slow for real time when the hot ones
(the per-voice filter, the I3DL2 reverb) are interpreted.

PLUGINS lists the bases seen at run time: `SPU_INTERP_STATS=10` names the hot
interpreted pages, and `SPU_INTERP_UNLIFTED=0 SPU_LS_DUMP=<prefix>` dumps the
core's local store when it stops at the first plugin call. ENTRIES lists the
plugin entry points the core calls through pointers (offsets from the image
base, i.e. link address 0), which --auto-functions cannot see.

Writes <spu-dir>/ms_dsp_<name>/spu_recomp.{c,h}; work/port/CMakeLists.txt picks
them up with the other lifted images. Run after work/tools/lift_spu.py (which
cleans <spu-dir>), or on its own:

  work/tools/lift_ms_dsp.py [--eboot ELF] [--toolkit DIR] [--spu-dir DIR]
"""
import argparse
import json
import os
import re
import struct
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]

# plugin note name -> LS base the core loads it to (image address 0 <-> base).
PLUGINS = {
    "mstream_dsp_filter.pic": 0x38700,
    "mstream_dsp_i3dl2.pic": 0x35E80,
    # Free-Play race (Kanaloa Bay), from SPU_INTERP_STATS and a local-store dump: the
    # interpreter ran ~30% of the MultiStream core thread on these three.
    "mstream_dsp_aux_return.pic": 0x35E80,
    "mstream_dsp_multi_compressor_TD.": 0x36300,   # the ELF note is truncated to 32 bytes
    "mstream_dsp_aux_send.pic": 0x37080,
}
# Entry points reached only through pointers, as offsets from the image base.
ENTRIES = {
    "mstream_dsp_filter.pic": [0x90, 0x3D0],
    "mstream_dsp_i3dl2.pic": [0x90, 0x98, 0xF0, 0x510, 0xCC0, 0xFF0],
    "mstream_dsp_aux_return.pic": [0x90, 0xF0],
    "mstream_dsp_multi_compressor_TD.": [0x90, 0xB70],
    "mstream_dsp_aux_send.pic": [0x90, 0xF0],
}
# Byte ranges (offsets from the plugin base, [start, end)) left out of the code check that
# gates dispatch. aux_send: the quadword at +0x980 (the head of a routine, `rchcnt $2,
# SPU_RdInMbox; brnz ...`) is overwritten with data once the plugin is installed, so a
# check of the whole .text never passed and the plugin stayed interpreted.
VERIFY_SKIP = {
    "mstream_dsp_aux_send.pic": [(0x980, 0x990)],
}
# Firmware decoders the core loads from dev_flash the same way (lifted from the
# user's own dev_flash, $PS3_DEV_FLASH, default ~/.config/rpcs3/dev_flash; skipped
# if absent): path under dev_flash -> (LS base, pointer-only entries).
EXTERNAL = {
    "sys/external/flashMP3.pic": (0x19780, [0x74A0]),
}
IMAGE_ID_BASE = 0x4D534400   # "MSD\0" + index


def be16(b, o): return struct.unpack_from(">H", b, o)[0]
def be32(b, o): return struct.unpack_from(">I", b, o)[0]


class Eboot:
    def __init__(self, path):
        self.d = Path(path).read_bytes()
        phoff, n = struct.unpack_from(">Q", self.d, 0x20)[0], be16(self.d, 0x38)
        self.segs = []
        for i in range(n):
            t, _fl, off, va, _pa, fs, _ms, _al = struct.unpack_from(">IIQQQQQQ", self.d, phoff + i * 56)
            if t == 1:
                self.segs.append((va, off, fs))

    def off_of(self, ea):
        for va, off, fs in self.segs:
            if va <= ea < va + fs:
                return off + ea - va
        raise KeyError(hex(ea))


def elf_extent(b):
    """Size of an SPU ELF from its headers (section table is last)."""
    end = be32(b, 0x20) + be16(b, 0x30) * be16(b, 0x2E)
    phoff, phn = be32(b, 0x1C), be16(b, 0x2C)
    for i in range(phn):
        o = phoff + i * 32
        end = max(end, be32(b, o + 4) + be32(b, o + 16))
    return end


def section(b, want):
    shoff, shent, shn, shstr = be32(b, 0x20), be16(b, 0x2E), be16(b, 0x30), be16(b, 0x32)
    so = be32(b, shoff + shstr * shent + 16)
    for i in range(shn):
        o = shoff + i * shent
        nm_off = so + be32(b, o)
        name = b[nm_off:b.index(b"\0", nm_off)].decode()
        if name == want:
            return be32(b, o + 12), be32(b, o + 16), be32(b, o + 20)   # addr, offset, size
    raise KeyError(want)


def run(cmd):
    print("+", " ".join(str(c) for c in cmd))
    if subprocess.run([str(c) for c in cmd]).returncode:
        sys.exit(f"[lift_ms_dsp] failed: {cmd[1] if len(cmd) > 1 else cmd[0]}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--eboot", default=str(ROOT / "disc/PS3_GAME/USRDIR/EBOOT.elf"))
    ap.add_argument("--toolkit", default=os.environ.get("PS3RECOMP_DIR") or str(ROOT / "ps3recomp"))
    ap.add_argument("--spu-dir", default=str(ROOT / "work/port/spu"))
    args = ap.parse_args()
    tools = Path(args.toolkit) / "tools"
    spu = Path(args.spu_dir)
    py = sys.executable
    eb = Eboot(args.eboot)
    manifest = json.loads((spu / "spu_manifest.json").read_text())
    plugins = {d["note"]: int(d["ea"], 16) for d in manifest["ms_dsp_handled"]}
    stage = spu / ".ms_dsp_stage"
    stage.mkdir(exist_ok=True)

    jobs = []   # (note, base, entries, elf bytes, origin)
    for note, base in sorted(PLUGINS.items()):
        if note not in plugins:
            sys.exit(f"[lift_ms_dsp] {note} not in {spu / 'spu_manifest.json'} (run lift_spu.py first)")
        off = eb.off_of(plugins[note])
        jobs.append((note, base, ENTRIES.get(note, []), eb.d[off:off + elf_extent(eb.d[off:off + 0x10000])],
                     f"EBOOT EA 0x{plugins[note]:X}"))
    flash = Path(os.environ.get("PS3_DEV_FLASH") or Path.home() / ".config/rpcs3/dev_flash")
    for rel, (base, entries) in sorted(EXTERNAL.items()):
        p = flash / rel
        if not p.exists():
            print(f"[lift_ms_dsp] {p} not found -- not lifted (runs interpreted)")
            continue
        jobs.append((Path(rel).name, base, entries, p.read_bytes(), f"dev_flash/{rel}"))

    for idx, (note, base, entries, elf, origin) in enumerate(jobs):
        name = re.sub(r"[^A-Za-z0-9]+", "_", note.removesuffix(".pic").removeprefix("mstream_dsp_"))
        tag = f"ms_dsp_{name}"
        img = IMAGE_ID_BASE + idx
        raw, reb = stage / f"{tag}.elf", stage / f"{tag}.rebased.elf"
        raw.write_bytes(elf)
        run([py, tools / "rebase_spu_elf.py", raw, reb, hex(base)])
        out = spu / tag
        out.mkdir(exist_ok=True)
        extra = ",".join(hex(base + e) for e in entries)
        run([py, tools / "spu_lifter.py", "--auto-functions", reb, "--symbol-prefix", f"{tag}_",
             "-o", out, "--source-name", "spu_recomp.c", "--header-name", "spu_recomp.h",
             "--return-entries", "--merge-chunks"] + (["--extra-funcs", extra] if extra else []))
        for f in ("spu_recomp.c", "spu_recomp.h"):
            p = out / f
            t = p.read_text(encoding="utf-8")
            t = re.sub(r'#include\s+"(?:\.\./)+runtime/spu/([A-Za-z0-9_]+\.h)"', r'#include "\1"', t)
            p.write_text(t, encoding="utf-8", newline="\n")
        taddr, toff, tsize = section(elf, ".text")
        text = elf[toff:toff + tsize]
        rows = ",\n".join("    " + ", ".join(f"0x{x:02X}" for x in text[i:i + 16])
                          for i in range(0, len(text), 16))
        # verified runs of .text: everything but VERIFY_SKIP (offsets from the base)
        cuts, pos = [], 0
        for a, b in sorted(VERIFY_SKIP.get(note, [])):
            a, b = max(a - taddr, 0), min(b - taddr, tsize)
            if a > pos: cuts.append((pos, a))
            pos = max(pos, b)
        if pos < tsize: cuts.append((pos, tsize))
        segs = ", ".join(f"{{ 0x{base + taddr + a:05X}u, 0x{b - a:X}u, {tag}_text + 0x{a:X} }}" for a, b in cuts)
        with open(out / "spu_recomp.c", "a", encoding="utf-8", newline="\n") as f:
            f.write(f"""
/* ---- appended by work/tools/lift_ms_dsp.py --------------------------------------
 * {note} ({origin}) lifted for LS base 0x{base:05X}: .text at
 * LS 0x{base + taddr:05X}, 0x{tsize:X} bytes. Registered as pinned image {img}: a branch
 * into the span makes it resident once local store holds these bytes there. */
#include "spu_overlay_residency.h"
extern void spu_begin_image(int image_id);

static const unsigned char {tag}_text[0x{tsize:X}] = {{
{rows}
}};

__attribute__((constructor)) static void {tag}_register(void)
{{
    spu_begin_image({img}); {tag}_spu_recomp_register(); spu_begin_image(0);
    const spu_overlay_code_segment seg[] = {{ {segs} }};
    spu_overlay_register_pinned(0x{base + taddr:05X}u, sizeof {tag}_text, {img}, seg, sizeof seg / sizeof seg[0]);
}}
""")
        print(f"[lift_ms_dsp] {note}: image {img} at LS 0x{base:05X} -> {out}")
    for p in stage.iterdir():
        p.unlink()
    stage.rmdir()


if __name__ == "__main__":
    main()
