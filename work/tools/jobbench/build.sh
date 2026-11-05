#!/usr/bin/env bash
# Build the replay harness against one lifted SPU module.
#   work/tools/jobbench/build.sh <module dir> <entry hex> <output binary> [extra compiler args...]
# e.g. build.sh work/port/spu/jobmod_67757f5f2148_e2490 4F80 /tmp/jb_4f80
# Extra args go to the compiler first, so `-I<dir>` with a modified spu_helpers.h in <dir> builds
# a variant against the same lifted code (compare its hashes and times with the stock build).
# Flags match the port's SPU sources (work/port/CMakeLists.txt): -O2 -march=x86-64-v3
# -ffp-contract=off -fno-strict-aliasing -fwrapv, SPU_USE_DISPATCH_MT.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
TK="${PS3RECOMP_DIR:-$ROOT/ps3recomp}"
mod=${1:?module dir}; entry=${2:?entry hex}; out=${3:?output}; shift 3
src="$mod/spu_recomp.c"; [ -f "$src" ] || { echo "no $src" >&2; exit 1; }
prefix=$(basename "$mod")
entry=$(printf "%08X" "0x$entry")
cc=${CC:-clang}
# the capture script's offsets must match this toolkit's spu_context
printf '#include "spu_context.h"\n#include <stddef.h>\n_Static_assert(sizeof(spu_context) == 0x41A60, "spu_context size changed: update capture.sh");\n_Static_assert(offsetof(spu_context, ls) == 0x40800, "spu_context.ls moved: update capture.sh");\n' \
    | $cc -std=gnu17 -fsyntax-only -I"$TK/runtime/spu" -I"$TK/include" -I"$TK/runtime/platform" -x c -
$cc -std=gnu17 -O2 -march=x86-64-v3 -ffp-contract=off -fno-strict-aliasing -fwrapv -w -DSPU_USE_DISPATCH_MT \
    "$@" -I"$TK/runtime/spu" -I"$TK/include" -I"$TK/runtime/platform" \
    -DREGFN="${prefix}_spu_recomp_register" -DENTRYFN="${prefix}_spu_func_${entry}" -DENTRY_PC=0x$entry \
    "$ROOT/work/tools/jobbench/jobbench.c" "$src" -o "$out" -lm
echo "built $out (${prefix}_spu_func_${entry})"
