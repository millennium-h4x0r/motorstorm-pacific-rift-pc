# work/tools

Helpers for lifting, testing and profiling the port. Paths are relative to the
repository root; every script finds the root itself. Most test scripts drive
an offscreen instance started with `INST=<name> work/run.sh` (see the
*Parallel test instances* section of [docs/NOTES.md](../../docs/NOTES.md)) and
write only under `work/inst/<name>/`, which is git-ignored.

## Lifting (run by `scripts/lift.sh`)

| Tool | What it does |
|---|---|
| `lift_spu.sh` | Wrapper: lifts the SPU code of your `EBOOT.elf` into `work/port/spu/` (`SPU_OUT` overrides). |
| `lift_spu.py` | Finds and lifts the SPU code by scanning the executable: the WWS policy module, the job modules, the SPU ELFs (Bink, MultiStream, zlib, Havok). `SPU_LIFT_EXTRA` passes extra `spu_lifter.py` options. |
| `lift_ms_dsp.py` | Lifts the MultiStream DSP plugins, and the firmware MP3 decoder from your `dev_flash` when present, at the local-store addresses the game loads them at. Called by `lift_spu.py`. |
| `patch_lift.py` | Splices the port's hooks (`work/port/port_patches.cpp`) into the lifted PPU code: the 60 fps timestep, frame-time compensation, chase-camera shake rate. Idempotent; fails if the lifted text is not what it expects. |

## Driving and measuring a running game

| Tool | What it does |
|---|---|
| `pad.sh` | Press a pad button through `PAD_FILE`, wait, and save the latest frame dump as a PNG (`INST=<name> work/tools/pad.sh cross 3`). Needs `ffmpeg`. |
| `to_race.sh` | Drive an instance from boot to a Free-Play race (default Kanaloa Bay, bike), checking frame dumps at each screen. `TRACK=n`, `TOD=n` and `VEHICLE=n` press RIGHT that many times on the Track and Time of Day rows and in Vehicle Select (e.g. `TRACK=3 VEHICLE=7`: Cascade Falls, monster truck). |
| `capture_race.sh` | Boot an instance, race (to_race.sh), hold the throttle from GO, and write every frame of a stretch to numbered PPMs; `SECS=0` only plays, `WINDOW=1` shows it. |
| `record.sh` | Record the game window (X11) with sound to `recordings/<time>.mp4` with ffmpeg. Linux only. |
| `lib.sh` | Sourced by the scripts above: finding and killing an instance, the bench lock, on Linux and macOS. |
| `finish_race.sh` | Soak: two full races, results, quit, back to the menu, with screenshots of each step. |
| `bench.sh` | The standard benchmark: boots an instance per round, races, and reports fps, frame-interval percentiles and (Linux only: `/proc`, gdb) CPU ms per frame per thread. The binary must be named `PacificRiftRecomp`. |
| `fliplog.py` | fps and frame-interval percentiles from a `PS3RECOMP_FLIPLOG` file. |

## Profiling

| Tool | What it does |
|---|---|
| [`tsample/`](tsample/README.md) | Unprivileged per-thread CPU sampler (`perf_event_open`) and `tsym.py`, which turns samples into a per-function histogram. Build it with `cc` as its README says; the binary is git-ignored. Linux only; on macOS use `/usr/bin/sample <pid> 4 1`. |
| [`jobbench/`](jobbench/README.md) | Captures entry states of one lifted SPU job function from a running game with gdb and replays them, to compare lifter or helper variants. The captures are game memory and stay in `work/inst/`. |

Nothing these tools produce from the game (lifted code, frame dumps, captures,
logs) may be committed; `scripts/check_clean.sh` enforces it.
