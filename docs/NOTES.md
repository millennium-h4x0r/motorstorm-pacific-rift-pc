# Technical notes

## EBOOT.elf (BCES-00129 01.00), first pass with ps3recomp v0.18.0

- PPU ELF64 big-endian, stripped, 2 load segments:
  text `0x10000` (filesz `0x1018ac8`), data `0x1030000` (filesz `0x2066c4`, memsz `0x4206a8`).
- Entry OPD `0x107b8d8` → code `0x10230`, TOC `0x109fbf0`.
- OPD table: 14,437 entries; `ppu_loader.py` finds **11,722 functions**.
- Imports: cellSpurs (39), sysPrxForUser (30), cellSysutil (29), sceNp2 (25),
  sys_net (25), cellGcmSys (25), sys_io (22), cellFont (16), sys_fs (15),
  cellVoice (13), sceNpTrophy (11), cellAudio (11), plus smaller modules.
- `extract_spu_images.py`: **26 embedded SPU ELF images** (927 KB),
  offsets `0x00F5A500` through `0x01115280`.

Generated files: `work/out/EBOOT.{functions,image,loader,imports}.json`,
`work/out/spu/`. Regenerate any time; they are not committed.
