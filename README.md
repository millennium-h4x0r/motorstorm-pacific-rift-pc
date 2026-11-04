# MotorStorm: Pacific Rift — native PC port (static recompilation)

**MotorStorm: Pacific Rift** (PS3, BCES-00129, version 01.00), statically
recompiled with [ps3recomp](https://github.com/sp00nznet/ps3recomp) v0.18.0.

> **No game files come with this project.** You need your own dump; the build
> turns **your** executable into code on **your** machine, and that code
> cannot be distributed.

## Status

| Area | State |
|---|---|
| Lift (PPU 39,469 functions; 9 SPU ELFs, WWS policy module, 31 job modules) and link | ✅ |
| Boot, intro FMV (Bink on SPU), first-run driver select, menus | ✅ |
| Free-Play race loads and runs (timer, AI, HUD, Havok physics on SPU) | ✅ |
| In-race 3D rendering | 🔧 corrupted (noise on terrain/vehicles), under investigation |
| Audio, saves, controllers (SDL pad works) | 🔧 untested end to end |
| Online | ❌ not planned |

Notes: [docs/NOTES.md](docs/NOTES.md). Game files: [docs/REQUIRED_FILES.md](docs/REQUIRED_FILES.md).

## Setup

```sh
scripts/setup.sh          # ps3recomp v0.18.0 + this port's patch, Python venv
scripts/lift.sh           # translates the PPU and SPU code of YOUR EBOOT.elf
scripts/build.sh          # compiles work/port/build/PacificRiftRecomp (Vulkan)
work/run.sh PS3RECOMP_VK_WINDOW=1   # play in a window (SDL pad)
```

## License

MIT for the code in this repository ([LICENSE](LICENSE)). MotorStorm: Pacific
Rift belongs to its owners (Evolution Studios / Sony Interactive
Entertainment). Research and preservation project.
