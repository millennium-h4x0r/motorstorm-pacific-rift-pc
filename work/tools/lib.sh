# Shared by the test drivers (sourced). Linux and macOS.

# Process ids of running games. Linux cuts a process name to 15 characters.
port_pids() { { pgrep -x PacificRiftReco; pgrep -x PacificRiftRecomp; } 2>/dev/null | sort -u; }

# pid_has_env <pid> <VAR=value>: whether that process was started with it.
pid_has_env() {
    if [ -r "/proc/$1/environ" ]; then
        tr '\0' '\n' < "/proc/$1/environ" 2>/dev/null | grep -qxF "$2"
    else
        ps -E -ww -o command= -p "$1" 2>/dev/null | tr ' ' '\n' | grep -qxF "$2"
    fi
}

# The instance whose pad file is <dir>/pad.txt: its pids, and killing it
# (SIGKILL: the game ignores SIGINT).
inst_pids() { local p; for p in $(port_pids); do pid_has_env "$p" "PAD_FILE=$1/pad.txt" && echo "$p"; done; }
kill_inst_dir() { local p; for p in $(inst_pids "$1"); do kill -KILL "$p"; done; }

# One measuring run at a time: hold <lockfile> on fd 9 until the script ends.
# Without flock(1) (macOS has none) the runs are simply not serialised.
bench_lock() {
    exec 9>"$1"
    if command -v flock >/dev/null; then
        flock -n 9 || { echo "waiting for $1 (another measuring run)" >&2; flock 9; }
    fi
}
bench_unlock() { if command -v flock >/dev/null; then flock -u 9; fi; }

# Seconds since the epoch with a fraction (date +%s.%N is GNU only).
now_s() { python3 -c 'import time; print("%.6f" % time.time())'; }
