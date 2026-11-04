# MotorStorm: Pacific Rift — native PC port (static recompilation)

**MotorStorm: Pacific Rift** (PS3, BCES-00129, version 01.00), statically
recompiled with [ps3recomp](https://github.com/sp00nznet/ps3recomp) v0.18.0.

> **No game files come with this project.** You need your own dump; the build
> turns **your** executable into code on **your** machine, and that code
> cannot be distributed.

## Status

| Area | State |
|---|---|
| Toolkit set up, executable analysed (11,722 PPU functions, 26 SPU images) | ✅ |
| Lift (PPU + SPU) and link | ⬜ |
| Boot, menus, races | ⬜ |

Notes: [docs/NOTES.md](docs/NOTES.md). Game files: [docs/REQUIRED_FILES.md](docs/REQUIRED_FILES.md).

## Setup

```sh
scripts/setup.sh          # ps3recomp at the pinned commit (+ port patch, if any), Python venv
.venv/bin/python ps3recomp/tools/ppu_loader.py disc/PS3_GAME/USRDIR/EBOOT.elf -o work/out
.venv/bin/python ps3recomp/tools/extract_spu_images.py disc/PS3_GAME/USRDIR/EBOOT.elf -o work/out/spu
```

## License

MIT for the code in this repository ([LICENSE](LICENSE)). MotorStorm: Pacific
Rift belongs to its owners (Evolution Studios / Sony Interactive
Entertainment). Research and preservation project.
