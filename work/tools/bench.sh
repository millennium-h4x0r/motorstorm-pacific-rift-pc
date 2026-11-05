#!/usr/bin/env bash
# Pack-racing benchmark: the standard measurement window.
#   [ROUNDS=3] [INST=bench] [BIN=...] work/tools/bench.sh [VAR=val ...]
# Each round boots one solo instance, drives it into a Free-Play race with
# to_race.sh, waits for GO (the race timer appears top right), holds R2 and
# measures the 10 s window starting 2 s after GO: 12 vehicles in view, before
# any crash. Prints fps, frame-interval p50/p90 and CPU ms per frame for main,
# the FIFO/RSX thread, the three busiest SPURS job workers and the MultiStream
# SPU thread, then the mean over the rounds. Extra VAR=val args go to the game.
# Kills the instance with SIGKILL after each round (the game ignores SIGINT).
# FTRACE=<prefix>: also record a PS3_FTRACE timeline of the window into
# <prefix>_r<round>.txt (ps3recomp/tools/ftrace_report.py).
# PROF_TRIGGER=<file>: created at the window's start and removed at its end,
# for a sampler preloaded through the game's environment arguments.
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
I="${INST:-bench}"; N="${ROUNDS:-3}"
D="$ROOT/work/inst/$I"; F="$D/frame.ppm"; FL="$D/flip.txt"; OUT="$D/bench.txt"
mkdir -p "$D"; : > "$OUT"
cd "$ROOT"

kill_inst() {
    for p in $(pgrep -x PacificRiftReco); do
        tr '\0' '\n' < /proc/$p/environ 2>/dev/null | grep -qx "PAD_FILE=$D/pad.txt" && kill -KILL $p
    done
}
inst_pid() {
    for p in $(pgrep -x PacificRiftReco); do
        tr '\0' '\n' < /proc/$p/environ 2>/dev/null | grep -qx "PAD_FILE=$D/pad.txt" && echo $p
    done | tail -1
}
# timer_sig: a signature of the race timer box (top right); it changes once the race clock runs
timer_sig() {
    python3 -I - "$F" <<'EOF2'
import sys, subprocess, hashlib
raw = subprocess.run(['ffmpeg', '-loglevel', 'error', '-i', sys.argv[1], '-vf', 'crop=iw*0.28:ih*0.1:iw*0.70:ih*0.03,scale=96:20',
                      '-f', 'rawvideo', '-pix_fmt', 'gray', '-'], capture_output=True).stdout
print(hashlib.md5(bytes(b >> 5 for b in raw)).hexdigest() if raw else 'none')
EOF2
}
# timer: yellow pixels in the race timer box (top right); > 150 once the box is shown
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
# classify threads from a few gdb snapshots: FIFO (frame_clock), job workers (spurs pm + wws/jobmod),
# MultiStream (the MultiStream SPU image 0e193327 / ms_dsp plugins)
classify() {
    local pid=$1
    for k in 1 2 3 4; do
        timeout 20 gdb -p "$pid" -batch -nx -ex 'set pagination off' -ex 'thread apply all bt 30' 2>/dev/null
        sleep 0.2
    done | awk '/^Thread/{ if (lwp) emit(); match($0, /LWP [0-9]+/); lwp=substr($0, RSTART+4, RLENGTH-4); s=""; next }
                /^#/{ s = s " " $0 }
                function emit() { if (s ~ /frame_clock/) print lwp, "fifo"; else if (s ~ /vk_ar_thread/) print lwp, "rec"; else if (s ~ /spu_0e193327|ms_dsp_/) print lwp, "ms";
                                  else if (s ~ /spurs_pm_worker/ && s ~ /jobmod|wws_pm/) print lwp, "job" }
                END{ if (lwp) emit() }' | sort -u
}

# One bench round at a time across everyone's bench.sh runs: a round holds
# work/inst/.bench.lock from boot to kill (concurrent instances skew results).
LOCK="$ROOT/work/inst/.bench.lock"
for r in $(seq 1 "$N"); do
    exec 9>"$LOCK"
    if ! flock -n 9; then echo "bench: waiting for $LOCK (another bench round is running)" >&2; flock 9; fi
    ok=0
    for try in 1 2 3; do                      # to_race.sh can miss a menu step: boot again
        kill_inst; wait 2>/dev/null; rm -f "$FL"; : > "$D/run.log"
        FT=(); [ -n "${FTRACE:-}" ] && { rm -f "${FTRACE}_r$r.txt"; FT=(PS3_FTRACE="-1:10:${FTRACE}_r$r.txt"); }
        env "$@" "${FT[@]}" INST="$I" TIMEOUT=900 PS3RECOMP_VK_DUMP_EVERY=10 PS3RECOMP_FLIPLOG="$FL" work/run.sh >/dev/null 2>&1 9>&- &
        sleep 3
        if INST="$I" timeout 600 work/tools/to_race.sh > "$D/to_race.txt" 2>&1; then ok=1; break; fi
        echo "round $r try $try: $(tail -1 "$D/to_race.txt")" >&2
    done
    [ $ok = 1 ] || { echo "round $r: to_race failed 3 times" | tee -a "$OUT"; kill_inst; wait; flock -u 9; continue; }
    pid=$(inst_pid)
    # GO: the race timer lights up (countdown is ~4 s after the flyover skip)
    # the timer box shows 00:00.00 during the countdown: GO is when its digits start changing
    go=0; for k in $(seq 1 80); do [ "$(timer_lit)" -gt 150 ] && { go=1; break; }; sleep 0.25; done
    if [ $go = 1 ]; then
        go=0; s0=$(timer_sig)
        for k in $(seq 1 80); do sleep 0.2; s1=$(timer_sig); [ "$s1" != "$s0" ] && { go=1; break; }; done
    fi
    [ $go = 1 ] || { echo "round $r: GO not seen" | tee -a "$OUT"; kill_inst; wait; flock -u 9; continue; }
    echo "0x0200 100000" > "$D/pad.txt"          # hold R2 from GO: stay with the pack
    t_go=$(date +%s.%N)
    cls=$(classify "$pid")                        # takes ~1-2 s, inside the 2 s lead-in
    python3 -I - "$pid" "$FL" "$t_go" "$r" "$cls" <<'EOF' | tee -a "$OUT"
import os, sys, time
pid, fl, t_go, rnd, cls = int(sys.argv[1]), sys.argv[2], float(sys.argv[3]), sys.argv[4], sys.argv[5]
roles = {}
for line in cls.split('\n'):
    p = line.split()
    if len(p) == 2: roles[int(p[0])] = p[1]
hz = os.sysconf('SC_CLK_TCK')
def cpu():
    d = {}
    for t in os.listdir('/proc/%d/task' % pid):
        try:
            f = open('/proc/%d/task/%s/stat' % (pid, t)).read().rsplit(')', 1)[1].split()
            d[int(t)] = (int(f[11]) + int(f[12])) / hz
        except Exception: pass
    return d
while time.time() < t_go + 2.0: time.sleep(0.01)
mono_us = lambda: time.clock_gettime(time.CLOCK_MONOTONIC) * 1e6   # the clock PS3RECOMP_FLIPLOG uses
if os.environ.get('FTRACE'):
    import signal; os.kill(pid, signal.SIGUSR2)   # opens the armed PS3_FTRACE window
pt = os.environ.get('PROF_TRIGGER')               # a sampler's trigger file: present during the window
if pt: open(pt, 'w').close()
lo = mono_us(); c0 = cpu()
time.sleep(10.0)
if pt: os.remove(pt)
c1 = cpu(); hi = mono_us()
ts = [int(l) for l in open(fl) if l.strip()]
w = [t for t in ts if lo <= t <= hi]
iv = sorted((b - a) / 1000.0 for a, b in zip(w, w[1:]))
n = len(iv)
fps = n / ((hi - lo) / 1e6) if n else 0.0
p = lambda q: iv[min(n - 1, int(q * n))] if n else 0.0
dc = {t: c1[t] - c0.get(t, 0) for t in c1}
frames = max(1, n)
def ms(t): return 1000.0 * dc.get(t, 0) / frames
jobs = sorted((t for t, r in roles.items() if r == 'job'), key=lambda t: -dc.get(t, 0))[:3]
fifo = [t for t, r in roles.items() if r == 'fifo']
msth = sorted((t for t, r in roles.items() if r == 'ms'), key=lambda t: -dc.get(t, 0))[:1]
parts = ['main %.1f' % ms(pid)]
parts.append('fifo %.1f' % (ms(fifo[0]) if fifo else -1))
parts.append('jobs ' + '/'.join('%.1f' % ms(t) for t in jobs))
rec = [t for t, r in roles.items() if r == 'rec']
if rec: parts.append('rec %.1f' % ms(rec[0]))
parts.append('ms %.1f' % (ms(msth[0]) if msth else -1))
print('round %s: %.1f fps  p50 %.1f p90 %.1f ms | cpu ms/frame: %s' % (rnd, fps, p(.5), p(.9), ', '.join(parts)))
EOF
    ffmpeg -loglevel error -y -i "$F" -vf scale=640:-1 "$D/bench_r$r.png"
    if [ -n "${FTRACE:-}" ]; then                     # the game writes the trace once the window closes
        for k in $(seq 1 60); do [ -s "${FTRACE}_r$r.txt" ] && grep -q "wrote" "$D/run.log" 2>/dev/null && break; sleep 0.5; done
        sleep 1
    fi
    kill_inst; wait
    flock -u 9
done
python3 -I - "$OUT" <<'EOF'
import re, sys
rows = [l for l in open(sys.argv[1]) if 'fps' in l and 'round' in l]
if not rows: print('bench: no valid rounds'); sys.exit(1)
num = lambda pat, l: float(re.search(pat, l).group(1))
keys = [('fps', r'([\d.]+) fps'), ('p50', r'p50 ([\d.]+)'), ('p90', r'p90 ([\d.]+)'),
        ('main', r'main ([\d.]+)'), ('fifo', r'fifo (-?[\d.]+)'), ('ms', r'ms (-?[\d.]+)$')]
out = []
for k, pat in keys:
    v = [num(pat, l.strip()) for l in rows]
    out.append('%s %.1f' % (k, sum(v) / len(v)))
print('bench mean over %d round(s): %s' % (len(rows), ', '.join(out)))
EOF
