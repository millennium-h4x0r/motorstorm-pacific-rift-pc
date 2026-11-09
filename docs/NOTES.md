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

## Resolution scale and FSR 1 upscaling (2026-11-07)

Player settings (`user/settings.ini`, written by the F1 menu's Video page; read by windowed runs only, and an environment variable already set wins):

    [video]
    scale = 1|2|3          ; PS3RECOMP_RES_SCALE: 720p / 1440p / 4K render targets (restart)
    upscaler = off|fsr     ; PS3RECOMP_FSR=1: AMD FSR 1 to the window's size (live)
    fsr_sharpness = 0.80   ; PS3RECOMP_FSR_SHARPNESS, 0..1, 1 sharpest (live)
    fps = 30|60            ; PS3RECOMP_FPS=60 (port_patches.cpp; restart)

- **Render scale** (`PS3RECOMP_RES_SCALE=N`, 1..4, engine path only; the approach is the Apocalypse port's `vk_res_scale`, hand-ported to this backend). Every colour and depth target the engine creates is N times its guest size. The engine still speaks guest pixels everywhere; `rsx_vulkan_backend.c` scales at the edges:
  - viewport, scissor, clear rectangles, snapshot extents (colour, corner, depth), and the depth-bias slope (times N, since depth changes 1/N as much per pixel);
  - uploads (target seed, NV3089 writes) go through a guest-size staging image and are blown up with a nearest blit;
  - readbacks (2D transfers, small-target exposure writeback, depth to guest) are shrunk on the GPU first: nearest for RGBA8 (may be packed data), linear for RGBA16F, nearest depth blit (CPU decimation if the depth format cannot blit);
  - the present blits the flipped surface into a full-size frame image (`frame`, N x 1280x720), which the window and the frame dump take.
  - WPOS: the GPU hands fragment programs image pixels. `rsx_fp_apply_wpos_scale` (rsx_fp_decompiler.c) inserts `input.position.xy *= fp_alpha.w` with fp_alpha.w = 1/N (new optional hook `rsx_draw_backend.res_scale`). Full-screen passes that build UVs from WPOS still sample at image resolution.
  - Textures stay at guest size.
- **FSR 1** (`PS3RECOMP_FSR=1`): EASU then RCAS as two compute passes on the present's command buffer, from the frame into an output the size of the swapchain (offscreen: `PS3RECOMP_FSR_OUT=<w>x<h>`, default 2x the frame). Skipped when the output is no larger than the frame. The shader math is AMD's unchanged `ffx_fsr1.h`/`ffx_a.h` (v1.20210629, MIT, `libs/video/vulkan/fsr1/LICENSE.txt`); `vulkan/rsx_vk_fsr.comp` is compiled to the committed `rsx_vk_fsr_{easu,rcas}.spv.h` with glslangValidator (command in its header), like the fallback shaders. The CPU constants come from AMD's `FsrEasuCon`/`FsrRcasCon` built with `A_CPU` (`rsx_vk_fsr1.c`). DLSS 2 and FSR 2/3 are temporal; they need motion vectors and a jittered projection, which this game does not produce.
- The present now copies only the centre pixel to the host unless a frame dump is due that frame (the full copy at 4K was 33 MB a frame).
- Verified (work/inst/ressh/, Kanaloa Bay Free-Play): scales 1, 2 and 3 render the same scene with matching exposure, shadows, motion blur and HUD. The flyover, crash replay, intro FMV and menus (to_race.sh passes) also render at 2x/3x. FSR 1 works at 1x to 4K and at 2x to 4K, and is visibly sharper than the bilinear stretch. fps not yet measured on a quiet machine.
- Known gaps: a shadow lookup with unnormalised coordinates (`rsx_fp_decompiler.c` _rsx_shadow, GetDimensions) would divide guest texel coordinates by the scaled size. Not seen in this game. Guest-memory round trips (readback then re-upload) come back at guest resolution, nearest-upscaled.

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

PAD_FILE holds (`libs/input/cellPad.c`): a line `<mask> [reports]` presses the mask for that many pad reports (default 40), and the file is read again only after the hold has run out. A long hold such as `0x0200 100000` therefore swallows every later press for about half an hour, and a `0` mask is not a release. To keep a button held while still being able to send menu input, rewrite a short hold periodically (`echo "0x0200 300" > pad.txt` every ~4 s) and let the last one drain (~5 s) before the next press. `work/tools/finish_race.sh` does this. It is also why the post-race replay can look hung: START brings back the results, X continues, and Retry Race / Quit Race appears a few seconds later.

`perf` isn't installed. As a sampling profiler, run gdb `thread apply all bt 1` in a loop and count the top frames.

## Performance

- The lifted SPU sources must be built with `-msse4.1` (work/port/CMakeLists.txt). Without it, `spu_helpers.h` falls back to the byte-loop `spu_shufb` called out of line. That was the top hotspot of the Bink decode threads and made the FMV stutter.
- Game speed (fixed 2026-11-06): races ran 2.05x too fast (race timer 37.1 s over 18.1 s of wall time).
  - Cause: the game paces itself at 30 fps. Its VBlankHandler (func_00522448) releases a GCM label once the vblank counter reaches the threshold at 0x011478BC (TOC-0x6A38). The RSX does an NV406E SEMAPHORE_ACQUIRE on that label before each flip. ps3recomp treats ACQUIRE as a no-op unless GCM_SEMA_ACQUIRE=1, so we flipped on every vblank (59.6/s).
  - Fix: main.cpp defaults GCM_SEMA_ACQUIRE=1 with GCM_SEMA_ACQUIRE_MS=250. The race timer now matches the wall clock and races hold 30 fps.
  - Every fps figure from before this fix (full about 65, pack about 57.6) is double-speed and vblank-capped, so it doesn't compare with current numbers.
  - No native 60 fps mode: PPU_POKE=011478BC:1 (threshold 1) gives about 55 fps, but the race timer runs 1.7-1.8x fast. The simulation steps a fixed 1/30 s per frame; the EBOOT has 38 f32 1/30 constants. A correct-speed 60 fps would need a timestep patch.
- 60 fps mode (`PS3RECOMP_FPS=60`, opt-in):
  - The game clock (f32 at 0x012FD140) advances by the frame timer's fixed step: n * 1e6 / 60 us, with n = vblanks per frame from SetInterval (func_00435D18, timer+0x18). The game asks for n = 2. The vblank gate's threshold (0x011478BC) is copied from the same field each frame.
  - work/tools/patch_lift.py (run by scripts/lift.sh) splices a call to port_timer_interval (work/port/port_patches.cpp) into func_00435D18. With PS3RECOMP_FPS=60 it turns 2 into 1, giving a 1/60 s step and a flip allowed every vblank.
  - Verified: the clock gains exactly 1/60 per flip. Without the hook, threshold 1 alone ran the race 1.75x fast. Expected fps is the old uncapped figure (about 65 full, about 58 pack on a quiet machine).
- SPU lift: work/tools/lift_spu.py passes `--const-bi` by default (SPU_LIFT_EXTRA overrides). Constant-target indirect branches become direct calls; measured +1.6 fps with the pack in view (idea credited to the Apocalypse lifter).

## Audio (2026-11-04)

- **Port 0 (MultiStream) was silent**: the MultiStream core (lifted taskset task,
  SPURS instance "Multistream") calls its DSP plugins at LS addresses it picks
  at run time. The toolkit ended the task on the first such call ("branched into
  unlifted LS 0x38790 ... ending the job"), so the PPU update thread waited on
  the core's event flag forever after the first ~32 updates. main.cpp now sets
  `SPU_INTERP_UNLIFTED=1` (`=0` restores the stop), and the interpreter stops at
  the call's link register so the lifted caller does not run its own code twice.
- **Speed**: interpreted, the plugins ran MultiStream at about 20% of real time.
  work/tools/lift_ms_dsp.py (run by lift_spu.py) lifts mstream_dsp_filter
  (LS 0x38700), mstream_dsp_i3dl2 (0x35E80) and dev_flash flashMP3.pic (0x19780)
  at those bases. They are registered with `spu_overlay_register_pinned` and
  dispatched only when local store holds the same .text bytes. A plugin loaded
  anywhere else is interpreted (the race loads one at 0x37080; it is cold).
  Port 0 now gets 1.53 MB/s, which is real time.
- The Linux mix loop is paced like the Windows one: device room via SDL (4
  blocks queued), CLOCK_MONOTONIC without a device. A peak limiter sits ahead
  of the clip, because the 7.1 fold-down overshoots full scale. `AUDIO_LIMITER=0` turns it off.
- Diagnostics: `AUDIO_PEAK=1`, `AUDIO_RATE=1`, `AUDIO_WAV=<file>` (raw f32le
  stereo 48 kHz), `PS3_MUTE=1`, `SPU_DMA_RANGE=58000000-58200000` (port 0
  PUT bytes per second, total and non-zero), `SPU_INTERP_STATS=<s>`.
- The lifted SPU sources build at `-O2 -march=${PORT_SPU_ARCH}` (default `x86-64-v3`, cache vars `PORT_SPU_ARCH` and `PORT_SPU_OPT`). Under `__AVX2__`, `spu_helpers.h` uses vector fa/fm/fma. `spu_context.h` reads and writes LS quadwords with a single pshufb under SSSE3 (`SPU_LS_NO_SSSE3` restores the old form). Together they cut the Bink video task from 44 to about 32 cpu-ms per frame. `-O3` was slower.
- The intro FMV is bound by the Bink video SPU task (taskset 0x0135D080, image b34579b4) on one host thread. Its decode capacity is only about 1.0-1.4x real time. Once it falls behind, the game decodes several frames per present and drops to 8-15 presents/s.
- The Bink task used to lose wakeups in `spu_taskset_wait_signal` (`runtime/spu/spu_workload.c`). An EventFlagSet that landed between the latch check and the park was latched and never consumed, and the bit test-then-clear could swallow a second Set. Both paths are fixed.
- `sys_lwcond_signal*` now wake `sys_lwcond_wait` immediately (`runtime/ppu/ppu_sysprx.cpp`). It used to be a bare Sleep(1); `PS3_LWCOND_SLEEP=1` restores that.
- Frame clock (`work/port/main.cpp`): vblank runs at 59.94 Hz on CLOCK_MONOTONIC, with a loop poll of at most 1 ms. It used to be 62.5 Hz with Sleep(4). Knobs: `PS3RECOMP_VBLANK_HZ`, `PS3RECOMP_CLOCK_POLL_US`.
- Since 2026-11-09 the vblank beat has a thread of its own (`vblank_clock`; `PS3RECOMP_VBLANK_THREAD=0` restores the old tick from frame_clock's loop). frame_clock also drains the FIFO, and in a 60 fps race one drain pass takes ~9.5 ms (p99 13.6). A beat due during a pass fired when the pass ended, so vblank intervals spread over 12-26 ms. The RSX waits on a label the game's vblank handler releases, so every late beat made a late frame. Now intervals are 16.7 ms and 60 fps mode holds 60 fps through the race's opening seconds. It still dips in the crash: SPU job image c9 adds ~1.1 s/s of job time, and main waits ~7.5 ms/frame on JobBatchHandler. PS3_FTRACE logs the beat as kind 22 and the guest vblank handler as kind 23.
- `PS3RECOMP_FLIPLOG=<file>` logs one µs timestamp per presented guest frame; `work/tools/fliplog.py <file> [start_s] [secs]` turns it into fps and interval percentiles.
- Race and flyover: `VK_DRAW_BATCH` is now on by default (`work/port/main.cpp`; `VK_DRAW_BATCH=0` turns it off). Without it every engine draw is a submit plus fence wait and the race ran at 1-3 fps. With it the race runs at 7-9 fps and the flyover at 12-20 fps. Batches are double-buffered (`VK_EB_ASYNC`, default on), and colour snapshots join the batch. `VK_SUBMIT_STATS=1` prints fence-wait time per call site; `VK_EB_OFF=<mask>` keeps clears (1), uploads (2), depth snapshots (4), draws (8) or colour snapshots (16) out of the batch.
- Race bottleneck, from PS3_FTRACE: the RSX FIFO drain is busy for about 130 of every 132 ms. About 20% of that is fence waits: 2-3 guest-reference flushes per frame (surfaces read back for format-reinterpreting textures and depth-to-guest) plus each draw's own render pass and full barrier on the GPU. The rest is walker CPU: driver command recording, vertex fetch/remap, texture decode/hash, and SPIR-V optimisation for new shaders. Main busy-spins in func_0050C818 waiting for SPU job counters.
- Render-pass merging (`VK_EB_MERGE`, default on): consecutive batched draws into the same framebuffer share a render pass, with a full barrier only on a sampling or attachment hazard. Race 6.8 -> 10.3 fps. `SPU_MFC_LOOKUP_CACHE=1` is defaulted in `main.cpp` (the toolkit's opt-in mfc_for memo): race 8.4 -> 10.6 fps.
- Race frame (about 65-75 ms at HEAD) is a latency chain: main computes about 20 ms, kicks SPU jobs, then waits about 30 ms for them. The SPU jobs run serially because each WWS job list (Wws_Job EvoHigh/EvoDefault/no name/LibMP3) was added with maxContention 1 (attribute +0x24); that is the game's own setting, and SPURS runs them the same way on hardware. More host threads cannot help. The hot job module is jobmod_67757f5f e2490 (func_4F80), so faster lifted job code is the lever.
- Main's job wait (func_004B63F0 at lr 0x0094CCA8 under func_0094CC80, a spinlock with a 250 us usleep back-off) waits for **JobBatchHandler**, not for an SPU flag. func_0094CC80(obj) takes and drops the locks at obj+0xC and obj+8, i.e. waits until both are free. The PPU thread JobBatchHandler (loop func_004AF670) holds obj+0xC while a batch is in flight and releases it (func_004AAF10 -> func_0094CBD8(obj, 0) -> func_004B63E0, a plain store; a gdb watchpoint sees it) in the pass after the batch's SPU jobs finish. The same passes submit the next batches (cellSpursReadyCountStore). Between passes it sleeps usleep(200) at lr 0x004AF6D0, ~250 us on Linux, so the poll period was on the critical path twice per batch. **Default `PS3_USLEEP_CAP=4af6d0:50`** (`work/port/main.cpp`, sys_timer.c): race ~51 -> ~59 fps with 6 lanes / maxcont 6, and main, FIFO and job CPU per frame all drop. Measured worse: `PS3_USLEEP_LOCKWAIT=94cca8:29` on top (main spins, ~53 fps), `PS3_USLEEP_WAKE=4af6d0` (the poll waits for a policy-module run that did work, 200 us timeout: ~54.5 fps; the pass also serves new requests from main, which no job completion signals), `PS3_USLEEP_PASSWAKE=94cca8:4af6d0` (main's back-off ends when JobBatchHandler reaches its next usleep: no gain over the cap), and a 20 us cap (falls under `PS3_USLEEP_SPIN_MAX`, noisier, no better). LOCKWAIT, WAKE and PASSWAKE stay opt-in.
- PPU stwcx./stdcx. commits now call the SPU line-commit hook (`ppu_loader.cpp`), so a PPU atomic on a SPURS request line wakes the coordinator at once instead of on its next 1 ms pass (`PPU_STWCX_KICK=0` turns it off). Race 13.0 -> 14.5 fps in one A/B, within noise. Polling the coordinator faster (100-200 us) made things much worse.
- SPU job worker in the race: the main WWS job list's policy module runs about 84% of wall time (`SPURS_DISPATCH_STATS=1`: about 3200 runs/s, 0.26 ms each, 0.05 ms gaps). Thread CPU splits as lifted job code about 65% (jobmod 67757f5f / 36bf670e / db10221f / 48b4ef08 ...), DMA memcpy about 13%, condvar traffic about 8% (cut by the coordinator's own condvar `kwake` in cellSpurs.c), and indirect dispatch about 6%. `SPU_FLOAT_RZ=1`, capping JobBatchHandler's poll (`PS3_USLEEP_CAP`), `PS3_USLEEP_LOCKWAIT` and the stwcx kick all measured neutral on race fps while the job lists ran serially; with the lists widened the poll cap is +15% (see the JobBatchHandler bullet).
- `work/tools/to_race.sh`: INST=<name>, drives a running instance to a Free-Play race with frame-dump checks.
- **Deviation from hardware: `SPURS_HOST_SPUS=6`** (default in `work/port/main.cpp`, `spurs_host_lanes` in `ps3recomp/libs/spurs/cellSpurs.c`). The game's 5-SPU SPURS instance gets 6 host policy-module lanes while the guest still sees nSpus=5; instances created with fewer than 5 SPUs (the game's 1-SPU one) keep their own count. 6 is the ceiling: the WWS job manager keeps a halfword per SPU at byte 4+2*spuNum of a quadword in the job list's atomic line (job claim at LS 0x2418 and the update at 0x343C), so a 7th SPU would overwrite the list's counters. Together with `SPURS_MAXCONT_MIN=6` (below, was 3): race 42.7 -> 51.4 fps, main's job wait 9.1 -> 6.0 ms/frame. Soak: two 9-minute race sessions (crash replays, respawns, laps 1-2), menus and the intro FMV clean at 55-61 fps. **`SPURS_HOST_SPUS=5 SPURS_MAXCONT_MIN=1`** restores hardware behaviour.
- Tried and dropped: reporting the 6 host lanes to the game (cellSpursGetInfo, its only SPU-count query, called 4 times at setup) does not change Havok's split. It still runs 5 tasks per physics phase (images 667/574/18 and 2x5 of 863), so the split comes from game-side taskset configuration, not the reported count.
- **Deviation from hardware: `SPURS_MAXCONT_MIN` (now 6, was 3)** (default in `work/port/main.cpp`, implemented in `ps3recomp/libs/spurs/cellSpurs.c` at AddWorkload). The game adds its three WWS job lists (Wws_Job, EvoDefault, EvoHigh) with maxContention=1 and never raises it, so on hardware each list runs on one SPU and all race jobs run serially. The override lets each list run on up to 3 virtual SPUs. The WWS job manager claims jobs atomically and orders them with its own barrier commands, which is what makes this work. Race 7.85 -> 13.45 fps (in-game race clock 7.93 s vs 14.43 s over the same wall time); 4 and 5 gave no more (13.75 vs 13.30). A 3-minute race soak (lap 0 to 2) was stable with correct visuals. If a job pair relied on implicit serial order, it would show as rare glitches or physics oddities. **`SPURS_MAXCONT_MIN=1` turns it off**; use it before blaming a glitch on anything else, and don't compare hang hunts across builds with and without it.
- `SPU_FLOAT_RZ=1` (default in main.cpp; ported from the toolkit's arm64-only mode to x86 in `ps3recomp/runtime/spu/spu_helpers.h` and `spu_channels.c`): SPU code runs with MXCSR at round toward zero, FTZ and DAZ, so fa/fm/fma are one native SSE op plus a range check instead of the exact widen-to-double emulation. Bit-identical: `test_spu_float_exact.c` passes all 447M lanes under the mode. Helpers test a thread-local flag set with MXCSR (a per-op stmxcsr cost about 4 ns). Race job CPU per frame dropped about 19%, and race fps rose 6-15% in same-binary A/Bs. `SPU_FLOAT_RZ=0` restores the exact forms.
- Vulkan backend, FIFO-thread costs (env switches, all default on, set to 0 to turn off):
  - `VK_EB_DEFER` defers destroying images/buffers/framebuffers until their batch's fence signals. Destroying them used to flush and wait.
  - `VK_IMG_POOL` recycles colour targets and depth-snapshot textures, and the snapshot pool grew to 64 entries with LRU eviction.
  - `VK_UBO_REUSE` lets a draw with no new constants reuse the previous UBO slice.
  - `VK_DSET_REUSE` reuses the descriptor set and skips redundant dynamic state between merged draws.
  - `VK_PRESENT_PIPELINE`: present submits without waiting, and the frame's completion moves to the next present (one frame of latency).
  - `VK_BE_PROFILE=1` prints the CPU time per engine hook every 2 s. New optional hooks `readback_async`/`readback_fetch` exist for the small-target writeback.
  - Race: about 33-36 fps, with the FIFO thread at about 52% CPU and about 40% waiting on the GPU (the per-frame small-target sync).
- The game ignores SIGINT. Stop test instances with SIGKILL, or they keep running and steal each other's pad-file presses.

## Profiling a thread

`work/tools/tsample` samples one thread's user-space IP at a fixed rate
(perf_event_open task clock, works unprivileged) and `tsym.py` turns the
samples into a per-function histogram against the executable. See its
README. A solo race profile of the main thread (2026-11-05): 53% lifted game
code (incl. 7% job-counter spin), 15% out-of-line VMX float helpers, 4% PPU
store coherence, 3% indirect-call dispatch, 2.4% HLE/syscalls.

### Road to 60 fps (status 2026-11-05)

Standard measurement: `work/tools/bench.sh` (10 s of pack racing from 2 s after GO, solo).
Current tree: ~35-38 fps in the pack (p50 ~27 ms). Per-frame CPU: FIFO/RSX ~19-22 ms
(the gate), main ~14.5, each of the 3 SPURS job workers ~12-15, MultiStream ~9-11.
Heavy frames issue ~3500-5500 draws; the FIFO thread costs ~6 us per draw.

Remaining structural steps (each multi-day):
1. Asynchronous backend proxy: wrap `rsx_draw_backend` so void calls (bind_*, draw,
   clear_*, present, release) are queued with copied payloads and recorded on a second
   thread. Handle-returning calls (surface_view ~80/frame, snapshots, texture/target
   create) must not drain the queue: allocate handles proxy-side and let the recorder
   bind them later. Removes ~8-10 ms/frame (backend draw recording) from the FIFO thread.
2. GPU-side vertex fetch: done, on by default (see below).
3. Main thread: ~7% is the job-counter spin (func_0050C818); per-job SPU speed and the
   PM-yield change (idle workers block instead of polling) are the levers.

Status update (later 2026-11-05): with GPU fetch, `SPURS_HOST_SPUS=6` + `SPURS_MAXCONT_MIN=6`,
`SPU_ASYNC_POOL=1` and `PS3_USLEEP_SPIN_MAX=20` (all defaults in main.cpp) the pack race runs
~51-57 fps (p50 ~18-19 ms). FIFO is off the critical path: main never waits on it except a
small RSX-reference wait (func_0072E1E0, usleep(30) on CellGcmControl.ref, 0.15-1.35 ms/frame).
The frame is main's: ~9.7 ms running (including the func_0050C818 job-counter spin), ~5.8 ms in
func_0094CC80 (waiting for a job object's two locks), ~2.4 ms on Havok taskset tasks (cellSync
mutex polls + event_queue_receive). The async backend proxy (`VK_ASYNC_RECORD=1`) gained
nothing at 30 fps and stays off there. Since 2026-11-09, 60 fps mode turns it on: once
the vertex pool moved to host memory, 1440p went from ~48 to 58.0 fps (3 rounds, p50 16.7 ms).
`VK_ASYNC_RECORD=0` turns it off. Timeline tools: `FTRACE=<prefix> work/tools/bench.sh`
(PS3_FTRACE armed by SIGUSR2 at GO+2 s; header base_ns = CLOCK_MONOTONIC) and
`PROF_TRIGGER=<file>` for a preloaded sampler.

### GPU-side vertex fetch (default on; `PS3RECOMP_ENG_GPUFETCH=0` turns it off)

The engine hands the GPU each draw's raw guest bytes instead of decoded vertices
(rsx_draw_engine.c, "GPU-side vertex fetch"; backend hooks fetch_pool / fetch_serial /
draw_fetch). It does not use a fixed-address mirror with per-page uploads. Pages in such a
mirror cannot be rewritten while earlier draws of the same frame still read them: copies
are not allowed inside a render pass, and the game rewrites its dynamic vertex rings within a
frame. Instead:
- Ranges are content-addressed, immutable blocks in a 128 MB pool in host-visible
  memory, so no copy commands are needed. Since 2026-11-09 the pool lives in host memory,
  not the BAR heap. CPU writes into the BAR ran at about 50 MB/s on the RTX 3090 while the
  GPU was busy. Copying about 0.2 MB per frame took 3-7 ms of RSX-thread time at 1440p,
  enough to hold 30 fps mode at about 22 fps. In host memory, 1440p runs at 29.7 fps in
  30 fps mode and about 37 -> 48 fps in 60 fps mode. `PS3RECOMP_GF_VRAM=1` tries the
  BAR first. A block is keyed by the per-frame
  range hash the vertex cache uses, including the SPU io_written generations.
- The pool is a ring. A block is reused only once fetch_serial says every draw that read it
  has completed, and a hot block is copied forward before the head reaches it.
- The vertex shader gets a generated preamble (eng_vs_to_fetch) that does the work the CPU
  used to do:
  - byteswaps and decodes all seven attribute types;
  - applies the base index and the frequency divider;
  - performs the quad/fan/polygon/quad-strip expansions per vertex, exactly as
    rsx_draw_engine_write_topology_indices does.
- The per-draw table (attribute addresses, formats, the viewport epilogue) is 176 bytes of
  VS push constants. Disabled attributes are stride-0 constant blocks.
- Indexed triangle strips (this game cuts them with 0xFFFF restarts) are host strips drawn
  from a byteswapped index block, with primitive restart on.
- Pull draws bind only the transform constants their program reads.
- Everything else takes the CPU path unchanged: inline or immediate vertices, batches that
  are not one contiguous run, and restart values other than all-ones.

Pack racing, 2026-11-05:
- ~100% of draws take the pull path.
- The engine profile's fetch phase drops 5.7-5.9 → 2.0-3.0 ms/frame.
- bench.sh FIFO CPU drops 15.6 → 11.3 ms/frame; fps is unchanged (~41).
- With the FIFO walker's engine-only fast feed (cellGcmSys.c gcm_word_engine_only), the FIFO
  thread sleeps ~40% of the race window. The frame is gated by the main thread's wait in
  func_004B63F0 (sys_timer_usleep), not by FIFO CPU.

The NVIDIA driver's pipeline disk cache stopped keeping entries at its default size, which a
few builds of this port fill. Every run then recompiled each race pipeline on the FIFO thread
(libnvidia-gpucomp 6% of it). main.cpp now sets `__GL_SHADER_DISK_CACHE_SIZE` to 10 GB unless
it is already set.
