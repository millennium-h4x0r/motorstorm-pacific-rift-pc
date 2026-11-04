#!/usr/bin/env bash
# Closed-loop input for a running session started with PAD_FILE=work/pad.txt
# and PS3RECOMP_VK_DUMP=work/frames/frame.ppm:
#   [HOLD=<reports>] work/tools/pad.sh <button> [wait-secs]   press (held HOLD reports, default 20), wait, write work/frames/now.png
# buttons: start up down left right cross circle square triangle l1 r1 select none
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
declare -A M=([select]=0x0001 [start]=0x0008 [up]=0x0010 [right]=0x0020 [down]=0x0040 [left]=0x0080
              [l2]=0x0100 [r2]=0x0200 [l1]=0x0400 [r1]=0x0800 [triangle]=0x1000 [circle]=0x2000
              [cross]=0x4000 [square]=0x8000 [none]=0)
b="${1:-none}"; [ "$b" != none ] && echo "${M[$b]} ${HOLD:-20}" > "$ROOT/work/pad.txt"
sleep "${2:-2}"
ffmpeg -loglevel error -y -i "$ROOT/work/frames/frame.ppm" -vf scale=640:-1 "$ROOT/work/frames/now.png"
echo "$ROOT/work/frames/now.png"
