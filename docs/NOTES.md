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
- `PS3RECOMP_FLIPLOG=<file>` logs one µs timestamp per presented guest frame; `work/tools/fliplog.py <file> [start_s] [secs]` turns it into fps and interval percentiles.
- Race and flyover: `VK_DRAW_BATCH` is now on by default (`work/port/main.cpp`; `VK_DRAW_BATCH=0` turns it off). Without it every engine draw is a submit plus fence wait and the race ran at 1-3 fps. With it the race runs at 7-9 fps and the flyover at 12-20 fps. Batches are double-buffered (`VK_EB_ASYNC`, default on), and colour snapshots join the batch. `VK_SUBMIT_STATS=1` prints fence-wait time per call site; `VK_EB_OFF=<mask>` keeps clears (1), uploads (2), depth snapshots (4), draws (8) or colour snapshots (16) out of the batch.
- Race bottleneck, from PS3_FTRACE: the RSX FIFO drain is busy for about 130 of every 132 ms. About 20% of that is fence waits: 2-3 guest-reference flushes per frame (surfaces read back for format-reinterpreting textures and depth-to-guest) plus each draw's own render pass and full barrier on the GPU. The rest is walker CPU: driver command recording, vertex fetch/remap, texture decode/hash, and SPIR-V optimisation for new shaders. Main busy-spins in func_0050C818 waiting for SPU job counters.
- Render-pass merging (`VK_EB_MERGE`, default on): consecutive batched draws into the same framebuffer share a render pass, with a full barrier only on a sampling or attachment hazard. Race 6.8 -> 10.3 fps. `SPU_MFC_LOOKUP_CACHE=1` is defaulted in `main.cpp` (the toolkit's opt-in mfc_for memo): race 8.4 -> 10.6 fps.
- Race frame (about 65-75 ms at HEAD) is a latency chain: main computes about 20 ms, kicks SPU jobs, then waits about 30 ms for them. The SPU jobs run serially because each WWS job list (Wws_Job EvoHigh/EvoDefault/no name/LibMP3) was added with maxContention 1 (attribute +0x24); that is the game's own setting, and SPURS runs them the same way on hardware. More host threads cannot help. The hot job module is jobmod_67757f5f e2490 (func_4F80), so faster lifted job code is the lever.
- Main's "spinlock" wait (func_004B63F0 from func_0094CC80/0094CBD8, usleep 250 us back-off) is really a job-done flag. Main sets word +0xC of the job object in func_0094CBD8 and waits for it to clear in func_0094CC80. A host watchpoint never sees the clear, so it comes through another mapping (SPU DMA). `PS3_USLEEP_LOCKWAIT=<lr list>:<reg>` (sys_timer.c) makes those back-offs return as soon as the word clears. It is not set by default: main.cpp used to set it, and it was removed after measuring about neutral while the job lists ran serially. The back-off is a direct `sc` (sys_timer_usleep, 250 us) inside func_004B63F0, which leaves LR untouched, so the list is the return addresses of the 73 `bl func_004B63F0` sites in work/src/recomp (`ctx->lr = 0x...; func_004B63F0(ctx)`), and the lock/flag word is in r29 (`:29`). `PS3_USLEEP_LOCKWAIT_STATS=1` prints freed/timeout counts.
- PPU stwcx./stdcx. commits now call the SPU line-commit hook (`ppu_loader.cpp`), so a PPU atomic on a SPURS request line wakes the coordinator at once instead of on its next 1 ms pass (`PPU_STWCX_KICK=0` turns it off). Race 13.0 -> 14.5 fps in one A/B, within noise. Polling the coordinator faster (100-200 us) made things much worse.
- SPU job worker in the race: the main WWS job list's policy module runs about 84% of wall time (`SPURS_DISPATCH_STATS=1`: about 3200 runs/s, 0.26 ms each, 0.05 ms gaps). Thread CPU splits as lifted job code about 65% (jobmod 67757f5f / 36bf670e / db10221f / 48b4ef08 ...), DMA memcpy about 13%, condvar traffic about 8% (cut by the coordinator's own condvar `kwake` in cellSpurs.c), and indirect dispatch about 6%. `SPU_FLOAT_RZ=1`, capping JobBatchHandler's poll (`PS3_USLEEP_CAP`), `PS3_USLEEP_LOCKWAIT` and the stwcx kick all measured neutral on race fps.
- `work/tools/to_race.sh`: INST=<name>, drives a running instance to a Free-Play race with frame-dump checks.
- **Deviation from hardware: `SPURS_MAXCONT_MIN=3`** (default in `work/port/main.cpp`, implemented in `ps3recomp/libs/spurs/cellSpurs.c` at AddWorkload). The game adds its three WWS job lists (Wws_Job, EvoDefault, EvoHigh) with maxContention=1 and never raises it, so on hardware each list runs on one SPU and all race jobs run serially. The override lets each list run on up to 3 virtual SPUs. The WWS job manager claims jobs atomically and orders them with its own barrier commands, which is what makes this work. Race 7.85 -> 13.45 fps (in-game race clock 7.93 s vs 14.43 s over the same wall time); 4 and 5 gave no more (13.75 vs 13.30). A 3-minute race soak (lap 0 to 2) was stable with correct visuals. If a job pair relied on implicit serial order, it would show as rare glitches or physics oddities. **`SPURS_MAXCONT_MIN=1` turns it off**; use it before blaming a glitch on anything else, and don't compare hang hunts across builds with and without it.
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

### GPU-side vertex fetch (default on; `PS3RECOMP_ENG_GPUFETCH=0` turns it off)

The engine hands the GPU each draw's raw guest bytes instead of decoded vertices
(rsx_draw_engine.c, "GPU-side vertex fetch"; backend hooks fetch_pool / fetch_serial /
draw_fetch). It does not use a fixed-address mirror with per-page uploads. Pages in such a
mirror cannot be rewritten while earlier draws of the same frame still read them: copies
are not allowed inside a render pass, and the game rewrites its dynamic vertex rings within a
frame. Instead:
- Ranges are content-addressed, immutable blocks in a 96 MB pool in the BAR heap
  (device-local and host-visible, so no copy commands). A block is keyed by the per-frame
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
