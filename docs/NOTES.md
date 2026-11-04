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

## Boot bring-up log (2026-11-04)

Each step below was a hard stop at boot, in order. Toolkit fixes live in
`work/patches/ps3recomp.patch` (regenerate with `scripts/make_patch.sh`).

1. **Audio output check** — "CAN'T FIND A SUPPORTED AUDIO RESOLUTION". The
   toolkit's `CellAudioOutSoundMode` was 4 bytes (real: 8, ends in a u32
   speaker layout), `CellAudioOutDeviceInfo` lacked its u16 latency, and
   `CellAudioOutState.soundMode` sat at +8 (real: +12). Fixed in
   `libs/misc/cellAvconfExt.[ch]`; GetState/GetConfiguration now report the
   mode the game configured (it configures 7.1 and then checks).
2. **malloc fails -> exit(1)** — fast-path HLE imports (lwmutex lock/unlock)
   returned before the import-stub TOC save to `0x28(r1)`; the caller's
   `ld r2,0x28(r1)` then loaded 0 and the CRT heap init read through a null
   TOC. Fixed in `runtime/ppu/ppu_hle.cpp` (save before the fast form).
3. **`/ms2.psarc` missing** — the dump split it into `ms2.psarc.666xx`;
   join them (docs/REQUIRED_FILES.md).
4. **FIOS loader deadlock** — `_cellSpursLFQueuePushBody` was not
   implemented, so the PPU's decompression requests never reached the SPU
   zlib task. Ported the Apocalypse port's LFQueue push + layout.
5. **Second deadlock, same queue** — the push woke the wrong taskset (every
   taskset had workload id 0; waiter ids are `(wid << 8) | task`). Each
   taskset now gets its own wid (8 + index per SPURS instance) written to
   the taskset and to the task's LS `0x1DC` (wklCurrentId).
6. **WWS policy module** at EA `0xFFCC80` (0x2D00 bytes, fp
   `0x093FC7EE6C9F7E84`) lifted as image 2 (work/tools/lift_spu.sh).

Now: Vulkan frames show the loading spinner; the main thread then loops
forever inside `func_00584C20` (under investigation). cellFont is missing
GetCharGlyphMetrics / GetKerning / InitLibraryFreeTypeWithRevision.

Useful switches: `PS3_HLE_TRACE=<n>`, `PS3_HLE_TOP=<secs>`, `PS3_SCTRACE=1`,
`SPURS_LFQ_LOG=1`, `PS3RECOMP_VK_DUMP=<file.ppm>` + `PS3RECOMP_VK_DUMP_EVERY=<n>`.
gdb: `gdb -p $(pgrep -x PacificRiftReco)`; lifted functions take the
`ppu_context*` in `$rdi`, `gpr[i] = ((unsigned long*)$rdi)[i]`, guest memory
at `(*(unsigned char**)&vm_base)+ea`.

## In-race rendering (2026-11-04)

- **Missing scene geometry**: the game fetches cellGcmGetOffsetTable at boot,
  maps its SPU command-list buffers (0x17100000 / 0x17500000) later, and turns
  list EAs into IO offsets through `ioAddress[ea >> 20]` itself. The guest copy
  of the table was published only by the GetOffsetTable call, so it read 0xFFFF
  and every CALL into an SPU-built draw list became 0xFFFFFFFF, which the walker
  skips. Fixed in `libs/video/cellGcmSys.c`: map and unmap now update the
  published copy as well. Draws per race frame went from about 55 to 1000-2900.
- **Noise**: the RSX fragment registers R[n] and H[2n]/H[2n+1] share storage. The
  velocity pass writes h0.xy and then does UP4UB r0.x. The decompiler kept the
  two files apart, so the velocity buffer came out as random bytes and the
  motion blur smeared them across the frame. Fixed in
  `libs/video/rsx_fp_decompiler.c` (alias sync for registers that cross files).
- **Overexposure**: the PPU exposure code (`func_004C3778`) samples the 16x16
  FP16 luminance target directly from local memory (EA 0xC7EBA600 + sample
  offsets). The engine kept the pixels on the GPU, so the code read zeros and
  drove the exposure (VP c[33].x, also the loading screen's c[59].w) to about
  35. Fixed in `libs/video/rsx_draw_engine.c` (`eng_small_targets_to_guest`):
  colour targets of at most `PS3RECOMP_ENG_SMALL_WRITEBACK` pixels (default
  1024) that were rendered in the frame are stored to guest memory at present.
  c[33].x now settles around 0.76.
- **Right/bottom smear**: the game scales its 3D resolution with load (1270x714
  down to about 725x408 while racing). It renders into the top-left corner of
  full-size targets and samples them as textures of the rendered size, at the
  target's pitch. The engine viewed the whole host target, so the image came
  out stretched, with the unrendered margins smeared across it. Fixed in
  `libs/video/rsx_draw_engine.c` (`eng_surface_corner`) together with a new
  optional backend hook `color_snapshot_rect`, implemented for Vulkan: a
  pitch-matched linear texture smaller than its surface samples a copy of that
  corner.
- Still open: the first race's loading screen still whites out over time, even
  with the exposure fix. The pause menu in a race now looks right.
- Diagnostics: `RSX_ENGINE_PASS_DUMP_TRIGGER=<file>` (start or re-arm a pass dump
  by creating the file), `RSX_ENGINE_DROP_LOG=1` (draw drop reasons),
  `RSX_CONST_LOG=33,33` (exposure constant uploads), `GCM_WORD_STATS=10`
  (call/ret counts in the FIFO).

## Parallel test instances

`INST=<name> work/run.sh` runs an isolated, offscreen instance. Each one gets its own:
- log: `work/inst/<name>/run.log`
- pad file: `work/inst/<name>/pad.txt`
- frame dump: `work/inst/<name>/frame.ppm`, written every 30 frames
- save directory: `work/inst/<name>/hdd0`, copied from `hdd0` on first use

Audio goes to the SDL dummy driver unless `INST_AUDIO=1`. `BIN` defaults to `work/port/build-<name>/` if that build exists, so you can A/B builds or env switches side by side:

    BUILD_DIR=work/port/build-exp JOBS=8 scripts/build.sh
    TIMEOUT=120 INST=exp  work/run.sh >/dev/null &
    TIMEOUT=120 INST=base BIN=work/port/build/PacificRiftRecomp work/run.sh >/dev/null &
    INST=exp work/tools/pad.sh cross 3      # -> work/inst/exp/now.png

`perf` isn't installed. As a sampling profiler, run gdb `thread apply all bt 1` in a loop and count the top frames.

## Performance

- The lifted SPU sources must be built with `-msse4.1` (work/port/CMakeLists.txt). Without it, `spu_helpers.h` falls back to the byte-loop `spu_shufb` called out of line. That was the top hotspot of the Bink decode threads and made the FMV stutter.
