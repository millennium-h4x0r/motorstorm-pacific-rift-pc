#!/usr/bin/env bash
# Race-finish coverage soak: boot -> race 1 (Kanaloa Bay, bike, 3 laps, R2 held) -> results ->
# Quit Race -> (unlock notice) -> Free-Play Game Setup -> laps 1 -> race 2 -> results -> Quit Race ->
# back out to the Wreckreation menu. Screenshots of every step go to $OUT (default
# work/inst/<INST>/shots); prints "alive at end" and error-like log lines. Holds bench.sh's lock.
#   [INST=fin] [BIN=...] [OUT=dir] work/tools/finish_race.sh
# Notes: PAD_FILE reads a new command only after the current hold has run out, so accelerate is
# held as refreshed short holds ("0x0200 300" every 4 s) and menus wait for the last one to drain.
# Holding R2 blind, the race ends ~10 min in (the AI finish, then the title gives up on us).
# In the post-race replay START brings the results back, X continues, and Retry Race / Quit Race
# appears a few seconds later.
cd "$(dirname "$0")/../.."
I="${INST:-fin}"; D=work/inst/$I; F=$D/frame.ppm; FL=$D/flip.txt
O="${OUT:-$D/shots}"; mkdir -p "$O"
export BIN="${BIN:-$PWD/work/port/build-perf/PacificRiftRecomp}"
exec 9>work/inst/.bench.lock; flock 9
shot(){ cp $F $O/s.ppm 2>/dev/null; ffmpeg -loglevel error -y -i $O/s.ppm -vf scale=480:-1 $O/$1.png 2>/dev/null; echo "  shot $1"; }
press(){ echo "$1 ${3:-8}" > $D/pad.txt; sleep $2; }
bar(){ cp $F $O/b.ppm 2>/dev/null; python3 -I - $O/b.ppm <<'PY'
import sys, subprocess
raw = subprocess.run(['ffmpeg','-loglevel','error','-i',sys.argv[1],'-vf','scale=640:360','-f','rawvideo','-pix_fmt','rgb24','-'],capture_output=True).stdout
if len(raw) < 640*360*3: print(0); sys.exit()
def px(x, y): return raw[(y*640+x)*3:(y*640+x)*3+3]
# results / menu help bar: warm light grey (r > b), not white surf or sky
n = sum(1 for y in range(308,323) for x in range(120,620) if min(px(x,y)) > 165 and px(x,y)[0] >= px(x,y)[2] + 15)
# the "Results" title banner, top left
t = sum(1 for y in range(28,46) for x in range(120,250) if min(px(x,y)) > 170)
# ...and no race timer: bright sky can fill the title strip during the race, but then the
# yellow timer digits are up top right; the results screen hides the HUD
yl = sum(1 for y in range(18,46) for x in range(440,620) if px(x,y)[0] > 200 and px(x,y)[1] > 140 and px(x,y)[2] < 90)
# ...and the yellow finish times of places 1-3 (a bright crash-cam backdrop fills the title strip too)
ty = sum(1 for y in range(55,145) for x in range(230,330) if px(x,y)[0] > 190 and px(x,y)[1] > 140 and px(x,y)[2] < 90)
print('%.2f' % (2.0 * t / (18*130.0) if yl < 50 and ty > 200 else 0.0))
PY
}
race_to_results(){ # $1 tag: hold R2 (refreshed short holds) until the results bar shows
  local t0=$(date +%s) k=0 b
  while :; do echo "0x0200 300" > $D/pad.txt; sleep 4; k=$((k+1)); [ $((k % 15)) = 0 ] && shot $1_t$(( $(date +%s) - t0 ))
    b=$(bar); python3 -c "import sys; sys.exit(0 if $b > 0.5 else 1)" && break
    [ $(( $(date +%s) - t0 )) -gt 3600 ] && { echo "$1: no results in 60 min"; return 1; }
    pgrep -f "$BIN /" >/dev/null || { echo "$1: GAME DIED"; return 1; }; done
  echo "$1: results after $(( $(date +%s) - t0 )) s"; : > $D/pad.txt; shot $1_results
  sleep 12; shot $1_results_drained    # let the last R2 hold run out (menus read the file only between holds)
}
quit_to_menus(){ # $1 tag: the results overlay hides itself into the replay (drained above);
  # in the replay START opens Retry Race / Quit Race (X only toggles the results, DOWN scrolls them)
  press 0x0008 3; shot $1_results_again   # START brings the results back
  press 0x4000 9; shot $1_menu            # X Continue hides them; Retry Race / Quit Race appears a few s later
  press 0x0040 2; shot $1_menu_quit_sel
  press 0x4000 6; shot $1_after_quit      # an unlock notice may come first ("X Continue")
  press 0x4000 7; shot $1_after_x2        # -> Free-Play Game Setup
}
mkdir -p $D; rm -f $FL
INST=$I TIMEOUT=3600 PS3RECOMP_VK_DUMP_EVERY=5 PS3RECOMP_FLIPLOG=$PWD/$FL work/run.sh >/dev/null 2>&1 9>&- &
sleep 3; INST=$I timeout 600 work/tools/to_race.sh 9>&- | tail -1; shot r1_start
race_to_results r1 || exit 1
quit_to_menus r1
# Game Setup: laps 3 -> 1, then opponents -> start -> vehicle (bike) -> load -> skip flyover
press 0x0040 1.5; press 0x0040 1.5; press 0x0080 1.5; press 0x0080 2; shot r2_setup_laps1
press 0x4000 4; shot r2_opponents
press 0x4000 5; shot r2_after_x2
press 0x4000 4; shot r2_after_x3
sleep 20; shot r2_flyover
press 0x4000 5; press 0x4000 4; shot r2_start
race_to_results r2 || exit 1
quit_to_menus r2
press 0x2000 4; shot r2_back1
press 0x2000 4; shot r2_back2
alive=$(pgrep -f "$BIN /" >/dev/null && echo yes || echo no); echo "alive at end: $alive"
grep -ciE "fatal|abort|segfault|assert|unhandled" $D/run.log | sed 's/^/error-like log lines: /'
cp $D/run.log $O/run.log; cp $FL $O/flip.txt
for p in $(pgrep -f "$BIN /"); do kill -9 $p; done
flock -u 9; echo DONE
