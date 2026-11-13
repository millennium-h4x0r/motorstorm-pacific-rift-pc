#!/usr/bin/env bash
# Capture every frame of a stretch of race play as numbered PPMs, for
# frame-by-frame analysis (camera motion, pacing).
#   [INST=cap] [SECS=6] [DELAY=2] [WINDOW=1] work/tools/capture_race.sh <out dir> [VAR=val ...]
# Boots an offscreen instance, drives it into a Free-Play race (to_race.sh),
# waits for GO, holds R2, and from DELAY s after GO writes every presented
# frame for SECS s to <out dir>/f<n>.ppm (PS3RECOMP_VK_SEQ, gated by a trigger
# file). Holds the bench lock. Extra VAR=val args go to the game, e.g.
# PS3RECOMP_FPS=60 PS3RECOMP_DT_MAX=1 (a fixed 1/60 s step per frame, so the
# dump's own cost cannot change what the game simulates). WINDOW=1 runs it in
# a window; SECS=0 captures nothing (just plays DELAY s of the race, e.g. to
# watch a log without bench.sh's gdb pauses).
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
OUTD="${1:?usage: capture_race.sh <out dir> [VAR=val ...]}"; shift
I="${INST:-cap}"; SECS="${SECS:-6}"; DELAY="${DELAY:-2}"
D="$ROOT/work/inst/$I"; F="$D/frame.ppm"; TRIG="$D/seq.on"
mkdir -p "$D" "$OUTD"; rm -f "$TRIG" "$OUTD"/f*.ppm
cd "$ROOT"
. "$ROOT/work/tools/lib.sh"
kill_inst() { kill_inst_dir "$D"; }
timer_sig() {
    python3 -I - "$F" <<'EOF'
import sys, subprocess, hashlib
raw = subprocess.run(['ffmpeg', '-loglevel', 'error', '-i', sys.argv[1], '-vf', 'crop=iw*0.28:ih*0.1:iw*0.70:ih*0.03,scale=96:20',
                      '-f', 'rawvideo', '-pix_fmt', 'gray', '-'], capture_output=True).stdout
print(hashlib.md5(bytes(b >> 5 for b in raw)).hexdigest() if raw else 'none')
EOF
}
timer_lit() {
    python3 -I - "$F" <<'EOF'
import sys, subprocess
raw = subprocess.run(['ffmpeg', '-loglevel', 'error', '-i', sys.argv[1], '-vf', 'scale=640:360',
                      '-f', 'rawvideo', '-pix_fmt', 'rgb24', '-'], capture_output=True).stdout
if len(raw) < 640 * 360 * 3: print(0); sys.exit()
n = 0
for y in range(12, 50):
    for x in range(450, 630):
        i = (y * 640 + x) * 3
        if raw[i] > 200 and raw[i + 1] > 150 and raw[i + 2] < 90: n += 1
print(n)
EOF
}
bench_lock "$ROOT/work/inst/.bench.lock"
ok=0
for try in 1 2 3; do
    kill_inst; : > "$D/run.log"
    env "$@" INST="$I" TIMEOUT=600 PS3RECOMP_VK_DUMP_EVERY=10 \
        PS3RECOMP_VK_SEQ="$OUTD/f%06u.ppm" PS3RECOMP_VK_SEQ_TRIGGER="$TRIG" \
        work/run.sh $([ "${WINDOW:-0}" = 1 ] && echo --window || echo --offscreen) >/dev/null 2>&1 9>&- &
    sleep 3
    if INST="$I" work/tools/to_race.sh > "$D/to_race.txt" 2>&1; then ok=1; break; fi
    echo "capture try $try: $(tail -1 "$D/to_race.txt")" >&2
done
[ $ok = 1 ] || { echo "capture: to_race failed" >&2; kill_inst; exit 1; }
go=0; for k in $(seq 1 80); do [ "$(timer_lit)" -gt 150 ] && { go=1; break; }; sleep 0.25; done
if [ $go = 1 ]; then
    go=0; s0=$(timer_sig)
    for k in $(seq 1 80); do sleep 0.2; s1=$(timer_sig); [ "$s1" != "$s0" ] && { go=1; break; }; done
fi
[ $go = 1 ] || { echo "capture: GO not seen" >&2; kill_inst; exit 1; }
echo "0x0200 100000" > "$D/pad.txt"
sleep "$DELAY"; [ "$SECS" != 0 ] && touch "$TRIG"; sleep "$SECS"; rm -f "$TRIG"
sleep 1; kill_inst; wait 2>/dev/null
echo "capture: $(ls "$OUTD"/f*.ppm 2>/dev/null | wc -l) frames in $OUTD"
