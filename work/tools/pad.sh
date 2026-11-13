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
mask() {   # no associative array: macOS ships bash 3.2
    case "$1" in
        select) echo 0x0001 ;; start) echo 0x0008 ;; up) echo 0x0010 ;; right) echo 0x0020 ;;
        down) echo 0x0040 ;; left) echo 0x0080 ;; l2) echo 0x0100 ;; r2) echo 0x0200 ;;
        l1) echo 0x0400 ;; r1) echo 0x0800 ;; triangle) echo 0x1000 ;; circle) echo 0x2000 ;;
        cross) echo 0x4000 ;; square) echo 0x8000 ;; *) echo 0 ;;
    esac
}
if [ -n "${INST:-}" ]; then
    D="$ROOT/work/inst/$INST"; PAD="$D/pad.txt"; IN="$D/frame.ppm"; OUT="$D/now.png"
else
    PAD="$ROOT/work/pad.txt"; IN="$ROOT/work/frames/frame.ppm"; OUT="$ROOT/work/frames/now.png"
fi
b="${1:-none}"; [ "$b" != none ] && echo "$(mask "$b") ${HOLD:-20}" > "$PAD"
sleep "${2:-2}"
ffmpeg -loglevel error -y -i "$IN" -vf scale=640:-1 "$OUT"
echo "$OUT"
