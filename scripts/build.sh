#!/usr/bin/env bash
# Configure and build the port into work/port/build.
#   JOBS=<n>  PORT_BUILD_MEM=<size>  (defaults 14 jobs, 80G memory scope)
#   PORT_VULKAN=ON|OFF  (default ON)
#   BUILD_DIR=<dir>  SPU_DIR=<dir>  (defaults work/port/build, work/port/spu)
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DIR="${PS3RECOMP_DIR:-$ROOT/ps3recomp}"
PORT="$ROOT/work/port"
BUILD="${BUILD_DIR:-$PORT/build}"
SPU_DIR="${SPU_DIR:-$PORT/spu}"
JOBS="${JOBS:-14}"
# Compiler: clang-20 when installed (+2 full / +4 pack fps over clang-18 on the
# same tree in an interleaved bench), else clang. Override with CC=/CXX=.
if [ -z "${CC:-}" ] && command -v clang-20 >/dev/null; then CC=clang-20; CXX=clang++-20; fi
CC="${CC:-clang}"; CXX="${CXX:-clang++}"
[ -f "$ROOT/work/src/recomp/ppu_recomp.h" ] || { echo "nothing lifted yet -- run scripts/lift.sh" >&2; exit 1; }
# A compiler change on an existing build dir makes CMake wipe its cache
# mid-configure and drop the -D values below; start such a dir fresh instead.
if [ -f "$BUILD/CMakeCache.txt" ]; then
    cached="$(sed -n 's/^CMAKE_CXX_COMPILER:[A-Z]*=//p' "$BUILD/CMakeCache.txt")"
    if [ -n "$cached" ] && [ "$(command -v "$CXX")" != "$cached" ] && [ "$CXX" != "$cached" ]; then
        echo "build.sh: compiler changed ($cached -> $CXX), reconfiguring $BUILD from scratch"
        rm -rf "$BUILD/CMakeCache.txt" "$BUILD/CMakeFiles"
    fi
fi
cmake -S "$PORT" -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_C_COMPILER="$CC" -DCMAKE_CXX_COMPILER="$CXX" \
    -DPS3RECOMP_DIR="$DIR" -DRECOMP_DIR="$ROOT/work/src/recomp" -DSPU_DIR="$SPU_DIR" \
    -DPS3RECOMP_RSX_VULKAN="${PORT_VULKAN:-ON}" \
    -DPython3_EXECUTABLE="${PYTHON:-$ROOT/.venv/bin/python}" >/dev/null
if command -v systemd-run >/dev/null && systemd-run --user --scope -q true 2>/dev/null; then
    systemd-run --user --scope -q -p MemoryMax="${PORT_BUILD_MEM:-80G}" -p MemorySwapMax=0 nice -n 10 \
        cmake --build "$BUILD" -j "$JOBS" -- -k 0
else
    nice -n 10 cmake --build "$BUILD" -j "$JOBS" -- -k 0
fi
echo "built: $BUILD/PacificRiftRecomp"
