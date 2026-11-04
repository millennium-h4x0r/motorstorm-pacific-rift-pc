#!/usr/bin/env bash
# Closed-loop input for a running session.
#   [INST=<name>] [HOLD=<reports>] work/tools/pad.sh <button> [wait-secs]
# Presses <button> (held HOLD reports, default 20), waits, and converts the
# latest frame dump to a PNG, printing its path.
#   With INST: drives an instance started by `INST=<name> work/run.sh`
#              (work/inst/<name>/pad.txt -> work/inst/<name>/now.png).
#   Without:   a session started with PAD_FILE=work/pad.txt and
#              PS3RECOMP_VK_DUMP=work/frames/frame.ppm (-> work/frames/now.png).
# buttons: start up down left right cross circle square triangle l1 r1 l2 r2 select none
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
declare -A M=([select]=0x0001 [start]=0x0008 [up]=0x0010 [right]=0x0020 [down]=0x0040 [left]=0x0080
              [l2]=0x0100 [r2]=0x0200 [l1]=0x0400 [r1]=0x0800 [triangle]=0x1000 [circle]=0x2000
              [cross]=0x4000 [square]=0x8000 [none]=0)
if [ -n "${INST:-}" ]; then
    D="$ROOT/work/inst/$INST"; PAD="$D/pad.txt"; IN="$D/frame.ppm"; OUT="$D/now.png"
else
    PAD="$ROOT/work/pad.txt"; IN="$ROOT/work/frames/frame.ppm"; OUT="$ROOT/work/frames/now.png"
fi
b="${1:-none}"; [ "$b" != none ] && echo "${M[$b]} ${HOLD:-20}" > "$PAD"
sleep "${2:-2}"
ffmpeg -loglevel error -y -i "$IN" -vf scale=640:-1 "$OUT"
echo "$OUT"
