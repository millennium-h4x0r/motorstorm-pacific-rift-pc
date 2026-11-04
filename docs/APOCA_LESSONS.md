# Lessons from the MotorStorm Apocalypse port

How the sibling port (`~/Games/RECOMP/motorstorm_apoca`) got to its main menu,
and what that means for Pacific Rift. That port uses ps3recomp v0.13.0
(`42f5f8f9`), a one-time copy of the inFAMOUS 2 port base (`b57ea21`) and a
3 MB patch (`work/patches/ps3recomp.patch`). Its git history has 11 commits.
"In v0.18" below comes from grepping `motorstorm_pacific/ps3recomp` (commit
`4e8f3ca8`). Its CHANGELOG only goes up to v0.15.0, so the code is the better
guide.

## A. Engine/toolkit fixes, roughly in boot order

1. **Havok can't start its tasks (Taskset2 API missing).**
   - Symptom: unresolved NIDs `0xC2ACDF43` (`_cellSpursTasksetAttribute2Initialize`)
     and `0x7FDF4FEF` (`cellSpursBarrierInitialize`). The Havok physics and
     animation tasks never start.
   - Fix (apoca f4d3c0d, `libs/spurs/cellSpurs.c`): `cellSpursCreateTaskset2`,
     `CreateTask2`, `JoinTask2`, `TryJoinTask2`, `DestroyTaskset2`,
     `BarrierInitialize` and `LFQueueDetachLv2EventQueue`, built on the v1
     taskset paths. Test: `libs/spurs/tests/test_spurs_taskset2.c`.
   - v0.18: Barrier is implemented. The Taskset2 functions and
     `LFQueueDetach` only appear in `nid_database.py`. **Not in v0.18.**
2. **Boot deadlock with the FIOS zlib task.**
   - Root cause: a SPURS-owned LFQueue woke the wrong taskset (the Havok one).
   - Fix (89f9759): each taskset gets its own workload id, written to LS
     `0x1DC`, and the LFQueue wakes the waiter's own taskset
     (`waiter id >> 8`). Test: `work/tests/boot_past_fios.sh`.
   - v0.18: **unclear.** It has per-taskset workload-id fields
     (`cellSpurs.c:3356`), but I didn't confirm the wake fix.
3. **EDGE MLAA asserts on its context save area size.**
   - Root cause: the size must be `0x400 + 0x800 * blocks` (MLAA asserts 3072
     for one block).
   - Fix (89f9759): formula changed in `cellSpurs.c`.
   - v0.18: **not in v0.18.** It uses `2048 + blocks * 2048`
     (`cellSpurs.c:3241,3316`), so MLAA would get 4096.
4. **`_cellSpursTaskAttributeInitialize` reads its arguments wrong.**
   - Root cause: the inFAMOUS "R8-form" heuristic. MotorStorm passes r7
     pointing at a `{ctx, size, lsPattern}` record.
   - Fix (89f9759): the descriptor form is taken automatically when that record
     looks well-formed.
   - v0.18: **likely, but opt-in.** Needs `SPURS_TASKATTR_DESC=1`
     (`cellSpurs.c:1222-1246`).
5. **lwcond signals lost.**
   - Root cause: the game signals a *copy* of `sys_lwcond_t`, and waiters were
     matched by struct address.
   - Fix (89f9759, `sysPrxForUser.c`): match waiters by `lwcond_queue` id.
   - v0.18: **likely** (`sysPrxForUser.c:688-720`).
6. **EDGE MLAA barrier hang at frame 2: three bugs** (8a90e41, apoca
   NOTES.md:404-446).
   - (a) `cellSpursCreateTask` did `taskset->taskCount++` on a native struct
     that overlaps the guest's big-endian `running` bitset, so every SPU-side
     signal was refused. Fix: every native store into the guest taskset
     removed. v0.18: likely fixed (no `taskCount` hits).
   - (b) Set/clear of the signalled bit raced against an SPU PUTLLC
     (lock-line store). Fix: `spurs_bitset_set/clear` now run under the
     lock-line stripe and call `spu_coh_notify_write`. v0.18: likely
     (`spurs_taskset.h`, `test_spurs_taskset_bits_atomic.c`).
   - (c) A guest PUTLLC never woke a task parked in WAIT_SIGNAL. Fix:
     `spu_taskset_line_committed`, called from `spu_channels.c`. v0.18:
     likely (present).
7. **SPU reservation registry overflowed.** Raised from 8 to 64 entries
   (3ecdb76). v0.18: unclear.
8. **Unresolved NIDs, stubbed (89f9759).**
   - `sys_prx_get_module_list` (`0xA5D06BF0`, built from
     `_sys_prx_get_module_list` plus `sys_prx_get_module_info`).
   - `cellVideoOutGetScreenSize` (`0xFAA275A4`).
   - `cellGameGetHomeDataExportPath` (`0x59B1EDE1`).
   - **None of the three is in v0.18.**
9. **Network and NP init (3ecdb76).**
   - `sceNpManagerGetAccountRegion`: in v0.18.
   - `cellSsl` certificate loader reading from dev_flash: present in v0.18.
   - `cellHttp` cookies: not found in v0.18.
   - NP runs signed out. `cellNetCtlGetState` answers NOT_INITIALIZED about
     7000 times a boot, and the game accepts that.
10. **cellFont (3ecdb76).**
    - Fix: guest-correct structs, VFS paths, FreeType rendering, guest
      allocator callbacks, float-argument ctx handlers.
    - v0.18: uses stb_truetype. Unclear whether its structs are right. Watch
      for empty glyphs in the menus.
11. **Movies (cellVdec/cellAdec) (4286dfe, apoca NOTES.md:667-692).** The game
    uses its own AVI demuxer, with XviD video (codec type 5) and AC3 audio.
    - `cellVdecQueryAttrEx` / `OpenEx`: in v0.18.
    - `cellAdecOpenEx` (`0x8B5551A4`): **not in v0.18.**
    - Handle 0 bug: the game treats handle 0 as "no decoder" and the movie
      stalls forever. Handles now start at 1. In v0.18 (`cellVdec.c:93`).
    - Picture queue that returns BUSY when full: in v0.18. v0.18 also has an
      ffmpeg decoder.
    - `CELL_VDEC_ERROR_FATAL` must be `0x80610180`: **still wrong in v0.18**
      (`cellVdec.h:27` has `0x80610107`).
12. **No save ever written, so every boot was a first boot (4286dfe).**
    - Root cause: the save-data ctx handlers were never registered, so
      `UserAutoLoad`/`UserAutoSave` lost the 9th argument (userdata at
      SP+112) and every autosave failed with ERROR_PARAM.
    - Fix: `ps3_savedata_register_ctx()` called from `ppu_hle_init`.
    - v0.18: **likely.** `cellSaveData_register_ctx_handlers()` is called at
      `ppu_sysprx.cpp:932`.
    - Also added: `PS3_SAVEDATA_ROOT` for test save dirs. Not in v0.18.
13. **Pad pressure.**
    - Bug: any digital bit with pressure under 255 was forced to 255, so every
      part-pulled trigger became full throttle.
    - Fix: force 255 only when pressure is 0.
    - v0.18: **not fixed** (`libs/input/cellPad.c:1083` still checks `< 255`).
    - The game enables pressure with `cellPadSetPortSetting(port, 0x6)`, not
      `SetPressMode`.
14. **Graphics (8a90e41).**
    - The Vulkan backend now honours `LD_FRAME_DUMP`. v0.18 has
      `PS3RECOMP_VK_DUMP` / `_DUMP_EVERY` instead.
    - Guest RSX programs on by default. The fallback drew flipped and offset
      frames at about 600 ms per frame. Use `PS3RECOMP_VK_GUEST_PROGRAMS=1`.
    - Still open: post-process colour target formats 10/12 are unsupported,
      and small 4 KB flip contexts use the immediate-flip fallback.
15. **`/dev_hdd1`** maps onto the disc root. The game reads hashed cache files
    there, and they miss. Give it its own host directory before anything
    writes there.
16. **Open blocker at milestone 4 (race load).** `hkp_integrate` waits forever
    in `rdch SPU_RdEventStat` with mask 0x400 (reservation lost) on a lock
    line nobody writes. Check that every store path raises the
    reservation-lost event.

## B. Apocalypse-specific (not reusable directly)

- SPU image EAs, fingerprints and image ids (apoca NOTES.md:156-363).
- EBOOT sha256 and `--code-end 0x13d0468`.
- Empty native tables in `ppu_native.cpp` / `ppu_native_entries.py`.
- `work/port/stubs.cpp` is the empty template.
- Pacific Rift needs all of these rediscovered from its own EBOOT.

## C. Launch setup (`work/play.sh`, `work/port/main.cpp`)

- **VFS root:** from the EBOOT path (`<root>/PS3_GAME/USRDIR/EBOOT.elf` gives
  `<root>`), or `PS3_VFS_ROOT` (`main.cpp:433-447`). The binary runs from the
  repo root with `disc/PS3_GAME/USRDIR/EBOOT.elf`. `disc/PS3_GAME` holds
  symlinks to the dump plus the decrypted EBOOT.elf.
- **hdd0:** `PS3_HDD0_ROOT`, default `<vfs_root>/../hdd0`, created if missing
  (`main.cpp:756-776`).
- **dev_flash:** `PS3_DEV_FLASH`, otherwise `<vfs_root>/../dev_flash`,
  otherwise RPCS3's copy in `~/.var/app/net.rpcs3.RPCS3/config/rpcs3/dev_flash`
  (Flatpak) or `~/.config/rpcs3/dev_flash` (`main.cpp:779-804`). v0.18's
  `ppu_fs.cpp` and `sys_fs.c` read the same variables.
- **Environment set by `play.sh`:**
  - `PS3RECOMP_VK_GUEST_PROGRAMS=1`, `PS3RECOMP_VK_WINDOW=1`
  - optional `PS3RECOMP_VK_PRESENT=fifo`, `PS3RECOMP_RES_SCALE`,
    `PS3RECOMP_VK_FULLSCREEN`
  - `SDL_NO_SIGNAL_HANDLERS=1`, `PS3RECOMP_ENG_FPSLOG=1`, `PS3RECOMP_FTLOG=1`
  - `PS3RECOMP_VK_DUMP` / `_DUMP_EVERY` (via `--dump N`)
  - `SPU_DUMP_MISS`, `SPU_DUMP_STUCK`, `SPURS_JC_TIMING`
  - `PS3_VERBOSE=0` (piped stderr otherwise floods)
  - `PS3_POLLTOP=10`
  - `PS3_STALL_MS=2500` / `PS3_STALL_DIR` (in-process stack dumps,
    `main.cpp:219-273`)
- **Hang watcher:** a background loop takes `eu-stack` dumps after 15 s
  without an fps line, and runs `perf record` on stalls or long slow stretches
  (`play.sh:69-160`).
- **Headless tests:** `SDL_AUDIODRIVER=dummy SDL_VIDEODRIVER=dummy timeout ...`
  plus a frame dump (`work/tests/first_frame.sh:24-26`).

## D. SPU lifting (`work/tools/lift_spu.py`)

- Everything is found by scanning the EBOOT; no addresses are typed in.
  - Every expected kind must be found or the script exits 1.
  - The output directory is cleaned first, and re-runs are byte-identical.
  - Needs `work/out/EBOOT.{imports,loader}.json`.
  - Lifts with `--local-regs --merge-chunks --const-bi`.
- **WWS Job Manager policy module:** wrapped at LS `0xA00`, registered as
  image 2 under its FNV fingerprint over the size rounded up to 128 bytes.
  Extra entry points come from the `bra` vector at LS 0 and the
  `bisl`/`brsl` return sites.
- **49 job modules (`C0DEC0DE`):** pulled out with `extract_jobmods.py --elf`,
  lifted at LS `0x4000` with `lift_jobmods.py`, registered by 16-byte header
  as images 200-248.
- **8 SPURS taskset ELFs (Havok, MultiStream, EDGE MLAA/zlib):** lifted with
  `build_spu_workloads.py`, registered by ELF and `.text` fingerprints as
  images 30-37.
- **17 MultiStream DSP `.pic` plugins:** position-independent, so not lifted.
  They run on the interpreter.
- No job chains, raw SPUs or SPU thread groups (the script checks).
- Details: apoca NOTES.md:145-402.

## E. Virtual pads and automated tests

These live in the patched `libs/input/cellPad.c` and are **not in v0.18**,
which only has `PAD_SCRIPT`, `PAD_FILE` and `PAD_AUTOPRESS`.

- **`PS3_PAD_FILE_<n>`:** held-state file per port, re-read every 4 ms. Tokens
  are button names, `NAME=pressure`, and `LX/LY/RX/RY`.
- **`PS3_PAD_SCRIPT`:** a timeline (`t=<s> port=<n> hold|set|release|connect|disconnect`).
  - The clock starts at the game's first `cellPadGetData`.
  - Holds count packets the game actually read.
  - Log lines start with `[pad]`.
  - Format and unit test: apoca NOTES.md:469-632, `test_pad_virtual.c`.
- **`work/tests/main_menu.sh`:**
  - A Python controller writes the held-state file and decides from the dumped
    frames: mean red in a region tells the title screen from the menu, and a
    panel-difference check confirms each D-pad press moved the cursor.
  - Two boots, each with a fresh `PS3_SAVEDATA_ROOT`.
  - PASS requires no `unresolved NID` in either log.
- **Suite:** `work/tests/run_all.sh` (reach_entry, boot_past_fios, first_draw,
  first_frame, main_menu).

## What to bring over first

1. Taskset2 HLE.
2. The context-save-size formula.
3. `SPURS_TASKATTR_DESC=1`, or make the descriptor form automatic.
4. The three unresolved-NID stubs plus `cellAdecOpenEx`.
5. The `CELL_VDEC_ERROR_FATAL` value.
6. The pad-pressure fix.
7. The virtual-pad system and `PS3_SAVEDATA_ROOT`.
8. The dev_flash/hdd0 defaults and the `play.sh` hang watcher.
