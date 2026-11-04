#!/usr/bin/env python3
"""Lift MotorStorm: Pacific Rift's SPU code into work/port/spu/, reproducibly.

Everything is found by scanning the decrypted EBOOT (BCES-00129 01.00); no
address below is typed in. Uses only upstream ps3recomp tools (v0.18.0). What
runs on the SPUs and how the port's runtime dispatches each kind:

  * WWS Job Manager policy module (PM): a raw SPU binary that the PPU adds as a
    SPURS workload. cellSpurs copies it to LS 0xA00 and looks it up by the
    FNV-1a-64 of the `sizePm` bytes the game passed. The runtime's WWS job
    handling (job-code buffer base at LS[0x1320], signature match of the
    staged module, job return path) is keyed to image id 2. It is found by its
    `stqa $r3,0x1320`, walking back to its 4-`ila` header. Its size is the data
    word after the word that holds its EA (0x2CB0); the game passes it rounded
    up to 128 bytes (sizePm=11520=0x2D00 in the runtime log, as in MotorStorm
    Apocalypse), so the fingerprint covers the rounded size.
  * WWS job modules (`4 ila | entry | size | 0 | 0 | C0DEC0DE`, linked at
    LS 0x4000), uncompressed in the EBOOT. The PM streams one into its job-code
    buffer and the runtime picks the lift by the module's 16-byte header. They
    are carved here exactly as lift_jobmods.find_engine_modules() carves them
    (imageSize - 32 bytes), then lifted with lift_jobmods.py --jobmods, which
    registers them by signature (images 200+). Two of them are the PT_LOAD
    contents of SPU ELFs 0020/0021, so they are also lifted as ELFs.
  * SPU ELFs (Bink, MultiStream core, EDGE zlib, 4 Havok tasks, plus the two
    wrapped job modules): build_spu_workloads.py --input, registered by
    fingerprint.
  * 17 MultiStream DSP plugins (`mstream_dsp_*.pic`, position-independent,
    linked at 0x80). The MultiStream core loads them at an LS address it picks
    at run time, so a lift at the link address could never be dispatched. They
    are passed as --handled (interpreter route), not lifted.

Usage: work/tools/lift_spu.py [--eboot ELF] [--toolkit DIR] [--spu-dir DIR]
Exits nonzero if any expected kind is missing or any tool fails. The output
directory is deleted and rebuilt on every run.
"""
import argparse
import hashlib
import json
import os
import re
import shutil
import struct
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
EXPECT_SHA256 = "bbb7fe2545e54847483439f59cda54e907db23a111d7afe9a1b655624886b287"
EXPECT_COUNTS = {"wws_pm": 1, "jobmod": 31, "elf": 26, "ms_dsp": 17}

PM_LSA = 0xA00
PM_IMAGE_ID = 2              # runtime/spu/spu_channels.c: WWS handling keyed to image 2
JOBMOD_BASE = 0x4000
STQA_R3_1320 = 0x20826403    # stqa $r3, 0x1320 (store job-code buffer base)

# Lifter options applied to every lift. --merge-chunks only changes speed: each
# run of chunks between call targets is one C function (every chunk stays
# registered for indirect entry).
MERGE = ["--merge-chunks"]


def die(msg):
    sys.exit(f"[lift_spu] ERROR: {msg}")


def run(cmd):
    print("+", " ".join(str(c) for c in cmd), flush=True)
    if subprocess.run([str(c) for c in cmd]).returncode:
        die(f"{os.path.basename(str(cmd[1]))} failed")


def be32(b, o):
    return struct.unpack_from(">I", b, o)[0]


def fnv1a64(b):
    # runtime/spu/spu_workload.c spu_workload_fingerprint(). NOTE: its basis is
    # the decimal 1469598103934665603 (0x14650FB0739D0383), not the canonical
    # FNV-1a basis 0xCBF29CE484222325; the registry must use the runtime's.
    h = 1469598103934665603
    for x in b:
        h = ((h ^ x) * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return h


class Eboot:
    def __init__(self, path):
        self.d = Path(path).read_bytes()
        d = self.d
        if d[:4] != b"\x7fELF" or d[4] != 2:
            die(f"{path} is not a decrypted ELF64 EBOOT")
        phoff, phnum = struct.unpack_from(">Q", d, 0x20)[0], struct.unpack_from(">H", d, 0x38)[0]
        self.segs = []
        for i in range(phnum):
            t, fl, off, va, _, fs, _, _ = struct.unpack_from(">IIQQQQQQ", d, phoff + i * 56)
            if t == 1 and fs:
                self.segs.append((off, va, fs, fl))

    def ea_of(self, off):
        for o, va, fs, _ in self.segs:
            if o <= off < o + fs:
                return va + off - o
        return None

    def off_of(self, ea):
        for o, va, fs, _ in self.segs:
            if va <= ea < va + fs:
                return o + ea - va
        return None

    def read(self, ea, n):
        o = self.off_of(ea)
        if o is None or self.off_of(ea + n - 1) is None:
            die(f"EA 0x{ea:X}+0x{n:X} is not file-backed")
        return self.d[o:o + n]

    def aligned_refs(self, value):
        pat, out = struct.pack(">I", value), []
        i = self.d.find(pat)
        while i >= 0:
            if i % 4 == 0 and self.ea_of(i) is not None:
                out.append(self.ea_of(i))
            i = self.d.find(pat, i + 1)
        return out


def is_ilas(b, o):
    return all(b[o + 4 * k] in (0x42, 0x43) for k in range(4))


# ---- discovery -----------------------------------------------------------------

def spu_note_name(elf):
    i = elf.find(b"SPUNAME\0")
    m = re.match(rb"[ -~]+", elf[i + 8:i + 72]) if i >= 0 else None
    return m.group(0).decode() if m else ""


def find_spu_elfs(eb, tools):
    sys.path.insert(0, str(tools))
    import extract_spu_images
    out = []
    for off, size in extract_spu_images.find_spu_images(eb.d):
        elf = eb.d[off:off + size]
        out.append(dict(off=off, ea=eb.ea_of(off), size=size, note=spu_note_name(elf),
                        sha256=hashlib.sha256(elf).hexdigest()))
    return out


def find_jobmods(eb):
    """[(ea, entry_off, raw)]: the same rule and carve as upstream
    lift_jobmods.find_engine_modules(), applied to the EBOOT's PT_LOAD segments
    (that function parses ELF32 program headers; the EBOOT is ELF64)."""
    out = []
    for o, va, fs, _ in eb.segs:
        blob = eb.d[o:o + fs]
        at = blob.find(b"\xc0\xde\xc0\xde")
        while at != -1:
            h = at - 0x20
            if h >= 0 and h % 16 == 0 and be32(blob, h + 0x28) == JOBMOD_BASE and is_ilas(blob, h):
                entry_off, image = struct.unpack_from(">II", blob, h + 0x10)
                size = image - 32
                if 0x30 <= entry_off < size and image <= 0x40000 - JOBMOD_BASE and h + size <= len(blob):
                    out.append((va + h, entry_off, bytes(blob[h:h + size])))
            at = blob.find(b"\xc0\xde\xc0\xde", at + 4)
    return sorted(out)


def find_wws_pm(eb):
    """4-ila header (not a job module) followed by `lqa $r1,...` within 16 KB
    before a `stqa $r3,0x1320`; size from the word after its EA word."""
    pat = struct.pack(">I", STQA_R3_1320)
    cands = set()
    for o, va, fs, fl in eb.segs:
        blob = eb.d[o:o + fs]
        i = blob.find(pat)
        while i >= 0:
            if i % 4 == 0:
                st = i & ~15
                while st >= max(0, i - 0x4000):
                    if is_ilas(blob, st) and blob[st + 0x20:st + 0x24] != b"\xc0\xde\xc0\xde" \
                            and be32(blob, st + 0x10) >> 23 == 0x061 and be32(blob, st + 0x10) & 0x7F == 1:
                        cands.add(va + st)
                        break
                    st -= 16
            i = blob.find(pat, i + 1)
    pms = []
    for ea in sorted(cands):
        for ref in eb.aligned_refs(ea):
            raw = be32(eb.read(ref + 4, 4), 0)
            if 0x100 <= raw <= 0x40000 - PM_LSA:
                pms.append(dict(ea=ea, ref=ref, raw_size=raw))
                break
    return pms


def pm_extra_entries(pm, lsa=PM_LSA):
    """Entries a static scan of the PM misses:
      * the interrupt-vector target: the PM assembles `bra imm` at LS 0 with
        `ilhu $rX,0x180 ; iohl $rX,imm` (0xA2C in inFAMOUS 2 / Apocalypse);
      * the return point after each `bisl $r0,$rA` (call into staged job code)
        and of each `brsl` before the next branch: jobs return indirectly."""
    words = [be32(pm, o) for o in range(0, len(pm) & ~3, 4)]
    out = set()
    for k, w in enumerate(words):
        if w >> 23 == 0x082 and (w >> 7) & 0xFFFF == 0x180:
            rx = w & 0x7F
            for j in range(k + 1, min(k + 8, len(words))):
                w2 = words[j]
                if w2 >> 23 == 0x0C1 and w2 & 0x7F == rx:
                    imm = (w2 >> 7) & 0xFFFF
                    if lsa <= imm < lsa + len(pm) and imm % 4 == 0:
                        out.add(imm)
                    break
        if w >> 21 == 0x1A9 and w & 0x7F == 0:
            out.add(lsa + 4 * k + 4)
            for j in range(k + 1, min(k + 9, len(words))):
                w2 = words[j]
                if w2 >> 23 == 0x066:
                    out.add(lsa + 4 * j + 4)
                if w2 >> 21 == 0x1A8 or w2 >> 23 == 0x064:
                    break
    return sorted(out)


def debase(path):
    p = Path(path)
    t = p.read_text(encoding="utf-8")
    t2 = re.sub(r'#include\s+"(?:\.\./)+runtime/spu/([A-Za-z0-9_]+\.h)"', r'#include "\1"', t)
    if t2 != t:
        p.write_text(t2, encoding="utf-8", newline="\n")


# ---- main ----------------------------------------------------------------------

def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--eboot", default=str(ROOT / "disc/PS3_GAME/USRDIR/EBOOT.elf"))
    ap.add_argument("--toolkit", default=os.environ.get("PS3RECOMP_DIR") or str(ROOT / "ps3recomp"))
    ap.add_argument("--spu-dir", default=str(ROOT / "work/port/spu"))
    args = ap.parse_args()

    tools = Path(args.toolkit).resolve() / "tools"
    if not (tools / "spu_lifter.py").exists():
        die(f"no toolkit at {args.toolkit}")
    py = sys.executable
    spu = Path(args.spu_dir).resolve()
    eb = Eboot(args.eboot)
    exact = hashlib.sha256(eb.d).hexdigest() == EXPECT_SHA256
    if not exact:
        print("[lift_spu] warning: not the BCES-00129 01.00 EBOOT; counts are only checked to be nonzero")

    # ---- discovery ---------------------------------------------------------------
    elfs = find_spu_elfs(eb, tools)
    dsps = [e for e in elfs if e["note"].startswith("mstream_dsp_")]
    mods = find_jobmods(eb)
    pms = find_wws_pm(eb)
    found = {"wws_pm": len(pms), "jobmod": len(mods), "elf": len(elfs), "ms_dsp": len(dsps)}
    print(f"[lift_spu] found: {found}")
    for kind, n in EXPECT_COUNTS.items():
        if (exact and found[kind] != n) or (not exact and kind != "ms_dsp" and found[kind] == 0):
            die(f"expected {n if exact else 'some'} of kind {kind}, found {found[kind]}")
    sigs = [raw[:16] for _, _, raw in mods]
    if len(set(sigs)) != len(sigs):
        die("two job modules share a 16-byte header: the runtime could not tell them apart")

    if spu.exists():
        shutil.rmtree(spu)
    stage = spu.parent / (spu.name + ".inputs")      # not inside spu (builder owns it)
    if stage.exists():
        shutil.rmtree(stage)
    stage.mkdir(parents=True)

    # ---- 1. SPU ELFs ---------------------------------------------------------------
    handled = {e["sha256"]: f"MultiStream DSP plugin {e['note']}: position-independent, loaded by "
                            f"the MultiStream core at a run-time LS address; runs interpreted"
               for e in dsps}
    (stage / "handled.json").write_text(json.dumps(handled, indent=2) + "\n")
    run([py, tools / "build_spu_workloads.py", "--input", args.eboot, "--lifted", spu,
         "--out", spu / "spu_workloads.c", "--constructor", "--title", "motorstorm_pacific_rift",
         "--handled", stage / "handled.json"] + MERGE)

    # ---- 2. job modules (by signature) ---------------------------------------------
    jm_dir = stage / "jobmods"
    jm_dir.mkdir()
    for ea, entry_off, raw in mods:
        (jm_dir / f"jobmod_{hashlib.sha1(raw).hexdigest()[:12]}_e{entry_off:04X}.bin").write_bytes(raw)
    run([py, tools / "lift_jobmods.py", "--jobmods", jm_dir, "--out", spu,
         "--register", spu / "jobmods_register.c", "--symbol-prefix", "pacific"] + MERGE)
    jm = json.loads((spu / "jobmods_manifest.json").read_text())
    if len(jm) != len(mods):
        die(f"lift_jobmods.py lifted {len(jm)} of {len(mods)} job modules")
    ea_by_sig = {raw[:16].hex(): ea for ea, _, raw in mods}
    for m in jm:
        m["ea"] = f"0x{ea_by_sig[m['sig']]:X}"
    (spu / "jobmods_manifest.json").write_text(json.dumps(jm, indent=2) + "\n")

    # ---- 3. WWS policy module (image 2) -----------------------------------------
    pm = pms[0]
    rounded = (pm["raw_size"] + 127) & ~127
    pm_bytes = eb.read(pm["ea"], rounded)           # lift the larger span
    fps = [fnv1a64(pm_bytes)]                       # over sizePm = rounded size
    extra = pm_extra_entries(pm_bytes[:pm["raw_size"]])
    print(f"[lift_spu] WWS PM at EA 0x{pm['ea']:X} (EA word at 0x{pm['ref']:X}), size 0x{pm['raw_size']:X}"
          f" (0x{rounded:X} rounded), fps {[f'0x{f:016X}' for f in fps]}, extra entries {[hex(a) for a in extra]}")
    (stage / "wws_pm.bin").write_bytes(pm_bytes)
    run([py, tools / "wrap_spu_elf.py", stage / "wws_pm.bin", "--base", hex(PM_LSA),
         "--entry", hex(PM_LSA), "--out", stage / "wws_pm.elf"])
    run([py, tools / "spu_lifter.py", "--auto-functions", stage / "wws_pm.elf", "--base", hex(PM_LSA),
         "--symbol-prefix", "wws_pm_", "-o", spu / "wws_pm", "--source-name", "spu_recomp.c",
         "--header-name", "spu_recomp.h", "--return-entries",
         "--extra-funcs", ",".join(hex(a) for a in sorted({PM_LSA} | set(extra))),
         "--report", stage / "wws_pm.report.json"] + MERGE)
    for f in ("spu_recomp.c", "spu_recomp.h"):
        debase(spu / "wws_pm" / f)
    if f"wws_pm_spu_func_{PM_LSA:08X}(" not in (spu / "wws_pm" / "spu_recomp.h").read_text():
        die("PM lift has no function at its entry 0xA00")

    regs = "".join(f'    spu_workload_register_img(0x{fp:016X}ULL, wws_pm_spu_func_{PM_LSA:08X}, '
                   f'{PM_IMAGE_ID}, "wws_pm");\n' for fp in fps)
    with open(spu / "spu_workloads.c", "a", encoding="utf-8", newline="\n") as f:
        f.write(f"""
/* ---- appended by work/tools/lift_spu.py ----------------------------------------
 * WWS Job Manager policy module: EA 0x{pm['ea']:X}, 0x{pm['raw_size']:X} bytes (0x{rounded:X}
 * rounded to 128), run by cellSpurs at LS 0x{PM_LSA:X}. Registered as image {PM_IMAGE_ID} (the
 * runtime's WWS job handling is keyed to it) under the fingerprint of the rounded
 * size, which is the sizePm the game passes and cellSpurs hashes. */
extern void wws_pm_spu_recomp_register(void);
extern void wws_pm_spu_func_{PM_LSA:08X}(spu_context*);

void port_wws_pm_register(void)
{{
    spu_begin_image({PM_IMAGE_ID}); wws_pm_spu_recomp_register();
{regs}    spu_begin_image(0);
}}

__attribute__((constructor)) static void port_wws_pm_register_ctor(void)
{{
    port_wws_pm_register();
}}
""")

    manifest = dict(
        eboot_sha256=hashlib.sha256(eb.d).hexdigest(),
        wws_pm=dict(ea=hex(pm["ea"]), size=hex(pm["raw_size"]), rounded=hex(rounded), ls=hex(PM_LSA),
                    image_id=PM_IMAGE_ID, fingerprints=[f"0x{f:016X}" for f in fps],
                    extra_entries=[hex(a) for a in extra]),
        jobmods=len(jm), elfs_lifted=len(elfs) - len(dsps),
        ms_dsp_handled=[dict(ea=hex(e["ea"]), note=e["note"], sha256=e["sha256"]) for e in dsps])
    (spu / "spu_manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    shutil.rmtree(stage)

    # ---- 4. MultiStream DSP plugins at their run-time LS bases (pinned lifts) -------
    run([py, ROOT / "work/tools/lift_ms_dsp.py", "--eboot", args.eboot, "--toolkit", args.toolkit,
         "--spu-dir", spu])
    n = len([d for d in spu.iterdir() if (d / "spu_recomp.c").exists()])
    print(f"[lift_spu] {n} lifted image(s) in {spu}: 1 PM, {len(jm)} job modules, "
          f"{len(elfs) - len(dsps)} ELFs ({len(dsps)} DSP plugins left to the interpreter)")


if __name__ == "__main__":
    main()
