#!/usr/bin/env bash
# Lift the game's SPU code from YOUR decrypted EBOOT into work/port/spu, where
# work/port/CMakeLists.txt picks it up (SPU_DIR: */spu_recomp.c, spu_workloads.c,
# jobmods_register.c). Nothing produced here may be shared: it is the game's
# code, translated. See work/tools/lift_spu.py for what is lifted and why.
#
#   work/tools/lift_spu.sh [path/to/EBOOT.elf]  (default: disc/PS3_GAME/USRDIR/EBOOT.elf)
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$ROOT"
PY="${PYTHON:-.venv/bin/python}"
exec "$PY" work/tools/lift_spu.py \
    --eboot "${1:-disc/PS3_GAME/USRDIR/EBOOT.elf}" \
    --toolkit "${PS3RECOMP_DIR:-ps3recomp}" \
    --spu-dir work/port/spu
