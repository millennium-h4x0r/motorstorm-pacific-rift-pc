#!/usr/bin/env bash
# Run the port. Logs to work/runs/<timestamp>.log as well as the terminal.
#   work/run.sh [extra env as VAR=val ...]
#   TIMEOUT=<secs> to stop after that long (for automated runs).
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="${BIN:-$ROOT/work/port/build/PacificRiftRecomp}"
mkdir -p "$ROOT/work/runs" "$ROOT/hdd0" "$ROOT/gamedata"
export PS3_TITLE="${PS3_TITLE:-MotorStorm: Pacific Rift}"
export PS3_DEV_FLASH="${PS3_DEV_FLASH:-$HOME/.config/rpcs3/dev_flash}"
export PS3_HDD0_ROOT="${PS3_HDD0_ROOT:-$ROOT/hdd0}"
export PS3_STDIO_UNBUFFERED=1
for kv in "$@"; do export "$kv"; done
log="$ROOT/work/runs/$(date +%Y%m%d-%H%M%S).log"
ln -sfn "$log" "$ROOT/work/runs/latest.log"
cd "$ROOT"
if [ -n "${TIMEOUT:-}" ]; then
    timeout -s INT -k 5 "$TIMEOUT" "$BIN" "$ROOT/disc/PS3_GAME/USRDIR/EBOOT.elf" 2>&1 | tee "$log"
else
    "$BIN" "$ROOT/disc/PS3_GAME/USRDIR/EBOOT.elf" 2>&1 | tee "$log"
fi
