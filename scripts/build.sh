#!/usr/bin/env bash
# Configure and build the port into work/port/build.
#   JOBS=<n>  PORT_BUILD_MEM=<size>  (defaults 14 jobs, 80G memory scope)
#   PORT_VULKAN=ON|OFF  (default ON)
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DIR="${PS3RECOMP_DIR:-$ROOT/ps3recomp}"
PORT="$ROOT/work/port"
BUILD="${BUILD_DIR:-$PORT/build}"
JOBS="${JOBS:-14}"
[ -f "$ROOT/work/src/recomp/ppu_recomp.h" ] || { echo "nothing lifted yet -- run scripts/lift.sh" >&2; exit 1; }
cmake -S "$PORT" -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
    -DPS3RECOMP_DIR="$DIR" -DRECOMP_DIR="$ROOT/work/src/recomp" -DSPU_DIR="$ROOT/work/port/spu" \
    -DPS3RECOMP_RSX_VULKAN="${PORT_VULKAN:-ON}" \
    -DPython3_EXECUTABLE="${PYTHON:-$ROOT/.venv/bin/python}" >/dev/null
if command -v systemd-run >/dev/null && systemd-run --user --scope -q true 2>/dev/null; then
    systemd-run --user --scope -q -p MemoryMax="${PORT_BUILD_MEM:-80G}" -p MemorySwapMax=0 nice -n 10 \
        cmake --build "$BUILD" -j "$JOBS" -- -k 0
else
    nice -n 10 cmake --build "$BUILD" -j "$JOBS" -- -k 0
fi
echo "built: $BUILD/PacificRiftRecomp"
