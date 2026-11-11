#!/usr/bin/env bash
# Drive a running instance from boot to a Free-Play race (Kanaloa Bay, bike) and skip the flyover.
#   INST=<name> [TRACK=n] [TOD=n] [VEHICLE=n] work/tools/to_race.sh   (start the instance first: INST=<name> work/run.sh &)
# Checks the frame dump at the key screens instead of trusting fixed waits:
#   - presses START until the main menu's help bar is up and "The Festival"/"Wreckreation" is lit,
#   - moves the cursor until WRECKREATION / FREE-PLAY is the highlighted (yellow) row,
#   - waits for the yellow loading screen to appear and to go away.
# Exit status 0 = in the race (flyover skipped), 1 = a step did not land (screen saved as now.png).
# Prints the loading time. Stop instances with SIGKILL; the game ignores SIGINT.
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
I="${INST:?INST=<name>}"
D="$ROOT/work/inst/$I"; F="$D/frame.ppm"; LOG="$D/run.log"

press() { echo "$1 ${3:-20}" > "$D/pad.txt"; sleep "$2"; }
# probe <what>: menu (help bar 0..1), peak (y of the highlighted menu row, 0 = none), yellow (% of loading-screen yellow)
probe() {
    python3 -I - "$F" "$1" <<'EOF'
import sys, subprocess
f, what = sys.argv[1], sys.argv[2]
W, H = (160, 90) if what == 'yellow' else (640, 360)
raw = subprocess.run(['ffmpeg', '-loglevel', 'error', '-i', f, '-vf', 'scale=%d:%d' % (W, H),
                      '-f', 'rawvideo', '-pix_fmt', 'rgb24', '-'], capture_output=True).stdout
if len(raw) < W * H * 3: print(0); sys.exit()
px = lambda x, y: raw[(y * W + x) * 3:(y * W + x) * 3 + 3]
if what == 'menu':      # the white help bar along the bottom of every menu screen
    n = sum(1 for y in range(327, 334) for x in range(120, 620) if min(px(x, y)) > 215)
    print('%.2f' % (n / (7 * 500.0)))
elif what == 'peak':    # row band with the most highlight-yellow in the menu column
    best = (0, 0)
    for y0 in range(170, 240, 3):
        n = 0
        for y in range(y0, y0 + 6):
            for x in range(110, 320):
                r, g, b = px(x, y)
                if r > 200 and g > 150 and b < 90: n += 1
        if n > best[0]: best = (n, y0)
    print(best[1] if best[0] > 120 else 0)
else:                   # yellow share of the "LOADING" controls screen
    n = sum(1 for i in range(0, len(raw), 3) if raw[i] > 190 and 120 < raw[i + 1] < 235 and raw[i + 2] < 90)
    print(int(100 * n / (W * H)))
EOF
}
fail() { echo "to_race: $*" >&2; ffmpeg -loglevel error -y -i "$F" -vf scale=640:-1 "$D/now.png"; exit 1; }

until grep -aq gui_intro_eu.bik "$LOG" 2>/dev/null; do sleep 1; done
sleep 20

# Title / attract -> main menu (The Festival or Wreckreation lit)
ok=0
for k in $(seq 1 20); do
    press 0x0008 6
    m=$(probe menu); p=$(probe peak)
    python3 -c "import sys; sys.exit(0 if 0.2 < float('$m') < 0.98 and 188 <= $p <= 218 else 1)" && { ok=1; break; }
done
[ $ok = 1 ] || fail "main menu not reached"
sleep 2
# Cursor onto WRECKREATION (highlight row ~206-218), then enter
# (peak 0 = no highlight visible right now, e.g. a menu transition: look again instead of pressing)
for k in $(seq 1 10); do p=$(probe peak); [ "$p" -ge 206 ] && [ "$p" -le 218 ] && break
    if [ "$p" = 0 ]; then sleep 1; else press 0x0040 3 4; fi; done
p=$(probe peak); [ "$p" -ge 206 ] && [ "$p" -le 218 ] || fail "Wreckreation not selected (peak $p)"
press 0x4000 7
# Wreckreation submenu: ONLINE is lit (~194-200); move down to FREE-PLAY (~207-220)
for k in $(seq 1 10); do p=$(probe peak); [ "$p" -ge 207 ] && [ "$p" -le 220 ] && break
    if [ "$p" = 0 ]; then sleep 1; else press 0x0040 3 4; fi; done
p=$(probe peak); [ "$p" -ge 207 ] && [ "$p" -le 220 ] || fail "Free-Play not selected (peak $p)"
# Free-Play -> game setup -> opponent setup (start race) -> vehicle select -> pick the bike.
# TRACK / TOD / VEHICLE = presses of RIGHT on the Track and Time of Day rows of
# Game Setup and in Vehicle Select (default 0: Kanaloa Bay, Morning, Bike).
right() { local i; for i in $(seq 1 "$1"); do press 0x0020 1.2 4; done; }
press 0x4000 6
if [ "${TRACK:-0}" != 0 ] || [ "${TOD:-0}" != 0 ]; then
    press 0x0040 1.2 4; right "${TRACK:-0}"                 # Game Type -> Track
    press 0x0040 1.2 4; press 0x0040 1.2 4; right "${TOD:-0}"   # -> Laps -> Time of Day
    sleep 2
fi
press 0x4000 6; press 0x4000 7
right "${VEHICLE:-0}"
press 0x4000 3

ok=0; for k in $(seq 1 20); do sleep 0.5; [ "$(probe yellow)" -gt 25 ] && { ok=1; break; }; done
# With the SPIR-V disk cache a warm load can be over before the first probe:
# carry on if the menu bar is gone (a menu screen would still show it).
if [ $ok = 0 ]; then
    m=$(probe menu)
    python3 -c "import sys; sys.exit(0 if float('$m') < 0.2 else 1)" || fail "no loading screen (a menu press did not land)"
    echo "to_race: no loading screen seen (fast load)"
fi
t0=$(date +%s)
for k in $(seq 1 180); do sleep 1; [ "$(probe yellow)" -lt 8 ] && break; done
echo "to_race: loading took $(( $(date +%s) - t0 + 1 )) s"
sleep 6
# Flyover "Press X to Skip": two presses, the first can land during a fade
press 0x4000 6; press 0x4000 6
ffmpeg -loglevel error -y -i "$F" -vf scale=640:-1 "$D/now.png"
echo "to_race: in race ($D/now.png). Hold accelerate with short refreshed holds, e.g. every 4 s: echo '0x0200 300' > $D/pad.txt (a long hold blocks later presses until it expires; see finish_race.sh)"
exit 0
