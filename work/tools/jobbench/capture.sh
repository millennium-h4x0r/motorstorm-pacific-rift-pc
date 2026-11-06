#!/usr/bin/env bash
# Capture entry states of one lifted SPU function from a running game instance.
#   work/tools/jobbench/capture.sh <pid|inst:NAME> <lifted-symbol> [count=4] [outdir]
# e.g. capture.sh inst:bench jobmod_67757f5f2148_e2490_spu_func_00004F80 4
# Breaks on the symbol with gdb `count` times (a short pause between hits so they are not all
# from one burst) and dumps the spu_context and the 256 KB local store at each entry:
# <outdir>/ctx_N.bin, ls_N.bin. outdir defaults to work/inst/jobbench/<symbol> -- game memory,
# which the repository's .gitignore keeps out. Get the instance where you want it first
# (e.g. a race: INST=jb work/run.sh &, INST=jb work/tools/to_race.sh). Don't do it while someone
# is running work/tools/bench.sh: gdb stops the game for each hit (the script takes bench.sh's
# lock, work/inst/.bench.lock, so it waits for a running bench round).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
target=${1:?pid or inst:NAME}; sym=${2:?lifted symbol}; n=${3:-4}
out=${4:-$ROOT/work/inst/jobbench/$sym}
pid=$target
if [[ $target == inst:* ]]; then
    want="PAD_FILE=$ROOT/work/inst/${target#inst:}/pad.txt"; pid=""
    for p in $(pgrep -x PacificRiftReco); do
        tr '\0' '\n' < /proc/$p/environ 2>/dev/null | grep -qx "$want" && pid=$p
    done
    [ -n "$pid" ] || { echo "no instance ${target#inst:} running" >&2; exit 1; }
fi
mkdir -p "$out"
# gdb stops the game at every hit: take the bench lock so this never overlaps a bench.sh round
# JB_HAVE_LOCK=1: the caller already holds it (e.g. a script that booted the instance under it)
if [ "${JB_HAVE_LOCK:-0}" != 1 ]; then
    exec 9>"$ROOT/work/inst/.bench.lock"
    flock -n 9 || { echo "capture: waiting for the bench lock" >&2; flock 9; }
fi
# spu_context layout: sizeof 0x41A60, gpr[128] at 0, the local-store pointer at 0x40800
# (checked by the harness build: build.sh asserts both).
cat > "$out/.capture.py" <<EOF
import gdb
gdb.execute("set pagination off")
for k in range($n):
    bp = gdb.Breakpoint("$sym", internal=True)
    gdb.execute("continue")
    ctx = int(gdb.parse_and_eval("\$rdi"))
    ls = int(gdb.parse_and_eval("*(unsigned long*)(%d + 0x40800)" % ctx))
    gdb.execute("dump binary memory $out/ctx_%d.bin %d %d" % (k, ctx, ctx + 0x41A60))
    gdb.execute("dump binary memory $out/ls_%d.bin %d %d" % (k, ls, ls + 0x40000))
    print("captured state %d" % k)
    bp.delete()
    gdb.execute("shell sleep 0.4")
gdb.execute("detach")
EOF
timeout 300 gdb -p "$pid" -batch -nx -x "$out/.capture.py" 2>&1 | grep -E "captured state|rror|No symbol"
rm -f "$out/.capture.py"
echo "states in $out"
