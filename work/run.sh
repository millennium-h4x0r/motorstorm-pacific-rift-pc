#!/usr/bin/env bash
# Run the port. Logs to work/runs/<timestamp>.log as well as the terminal.
#   work/run.sh [extra env as VAR=val ...]
#   TIMEOUT=<secs> to stop after that long (for automated runs).
#   PS3RECOMP_VK_WINDOW=1 to show a window (rendering is offscreen by default).
#
# Parallel test instances: INST=<name> isolates everything a run writes, so
# several builds/configurations can run side by side (offscreen):
#   work/inst/<name>/run.log      log (no work/runs/latest.log repointing)
#   work/inst/<name>/pad.txt      PAD_FILE  (drive with INST=<name> work/tools/pad.sh)
#   work/inst/<name>/frame.ppm    PS3RECOMP_VK_DUMP, every PS3RECOMP_VK_DUMP_EVERY (default 30) frames
#   work/inst/<name>/hdd0         private copy of hdd0 (saves), seeded from hdd0 on first use
#   BIN defaults to work/port/build-<name>/PacificRiftRecomp when that exists.
#   Audio goes to SDL's dummy driver unless INST_AUDIO=1.
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
for kv in "$@"; do export "$kv"; done
INST="${INST:-}"
if [ -n "$INST" ]; then
    I="$ROOT/work/inst/$INST"
    mkdir -p "$I"
    [ -d "$I/hdd0" ] || { mkdir -p "$ROOT/hdd0"; cp -a "$ROOT/hdd0" "$I/hdd0"; }
    [ -x "$ROOT/work/port/build-$INST/PacificRiftRecomp" ] && BIN="${BIN:-$ROOT/work/port/build-$INST/PacificRiftRecomp}"
    export PS3_HDD0_ROOT="${PS3_HDD0_ROOT:-$I/hdd0}"
    export PAD_FILE="${PAD_FILE:-$I/pad.txt}"
    export PS3RECOMP_VK_DUMP="${PS3RECOMP_VK_DUMP:-$I/frame.ppm}"
    export PS3RECOMP_VK_DUMP_EVERY="${PS3RECOMP_VK_DUMP_EVERY:-30}"
    [ "${INST_AUDIO:-0}" = 1 ] || export SDL_AUDIODRIVER="${SDL_AUDIODRIVER:-dummy}"
    : > "$PAD_FILE"
    log="$I/run.log"
else
    mkdir -p "$ROOT/work/runs"
    log="$ROOT/work/runs/$(date +%Y%m%d-%H%M%S).log"
    ln -sfn "$log" "$ROOT/work/runs/latest.log"
fi
BIN="${BIN:-$ROOT/work/port/build/PacificRiftRecomp}"
mkdir -p "$ROOT/hdd0" "$ROOT/gamedata"
export PS3_TITLE="${PS3_TITLE:-MotorStorm: Pacific Rift}"
export PS3_DEV_FLASH="${PS3_DEV_FLASH:-$HOME/.config/rpcs3/dev_flash}"
export PS3_HDD0_ROOT="${PS3_HDD0_ROOT:-$ROOT/hdd0}"
export PS3_STDIO_UNBUFFERED=1
export PS3RECOMP_VK_GUEST_PROGRAMS="${PS3RECOMP_VK_GUEST_PROGRAMS:-1}"
cd "$ROOT"
if [ -n "${TIMEOUT:-}" ]; then
    timeout -s INT -k 5 "$TIMEOUT" "$BIN" "$ROOT/disc/PS3_GAME/USRDIR/EBOOT.elf" 2>&1 | tee "$log"
else
    "$BIN" "$ROOT/disc/PS3_GAME/USRDIR/EBOOT.elf" 2>&1 | tee "$log"
fi
