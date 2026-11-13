#!/usr/bin/env bash
# Lift the game's code from YOUR decrypted executable into work/: the PPU
# into work/src/recomp, the SPU images into work/out/spu. Nothing produced
# here may be shared: it is the game's code, translated.
#
#   scripts/lift.sh [path/to/EBOOT.elf]   (default: disc/PS3_GAME/USRDIR/EBOOT.elf)
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DIR="${PS3RECOMP_DIR:-$ROOT/ps3recomp}"
PY="${PYTHON:-$ROOT/.venv/bin/python}"
ELF="${1:-$ROOT/disc/PS3_GAME/USRDIR/EBOOT.elf}"
# The addresses in this port are those of BCES-00129 (Europe) version 01.00.
EXPECT=bbb7fe2545e54847483439f59cda54e907db23a111d7afe9a1b655624886b287

[ -f "$ELF" ] || { echo "no decrypted executable at $ELF (see docs/REQUIRED_FILES.md)" >&2; exit 1; }
[ -d "$DIR/tools" ] || { echo "no toolkit at $DIR -- run scripts/setup.sh" >&2; exit 1; }
if command -v sha256sum >/dev/null; then got=$(sha256sum "$ELF" | cut -c1-64)
else got=$(shasum -a 256 "$ELF" | cut -c1-64); fi
if [ "$got" != "$EXPECT" ]; then
    echo "warning: $ELF is not the BCES-00129 01.00 executable this port was made for" >&2
    echo "         (sha256 $got); the lift may build but the port will not run it" >&2
fi

name=$(basename "$ELF"); name="${name%.*}"
rm -rf "$ROOT/work/src/recomp"
mkdir -p "$ROOT/work/out" "$ROOT/work/src"
"$PY" "$DIR/tools/ppu_loader.py" "$ELF" -o "$ROOT/work/out"
# .text ends at 0xcc0ddc (end of the last AX section); above it is .rodata.
"$PY" "$DIR/tools/ppu_lifter.py" "$ELF" \
    --functions "$ROOT/work/out/$name.functions.json" \
    --hle-stubs "$ROOT/work/out/$name.imports.json" \
    --code-end 0xcc0ddc --chunks 48 --output "$ROOT/work/src/recomp"
# The port's hooks into the game's own code (work/tools/patch_lift.py).
"$PY" "$ROOT/work/tools/patch_lift.py" "$ROOT/work/src/recomp"
"$ROOT/work/tools/lift_spu.sh" "$ELF"
echo "lifted: work/src/recomp (PPU), work/port/spu (SPU)"
