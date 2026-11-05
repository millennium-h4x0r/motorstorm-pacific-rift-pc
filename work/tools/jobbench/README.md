# jobbench: replay benchmark for lifted SPU functions

Frame-level benches (`work/tools/bench.sh`) can't resolve a few-percent change in SPU job code:
other threads swing the frame by ±25% between rounds. jobbench times one lifted SPU function on
real inputs, without the game running:

1. **capture** the `spu_context` and the 256 KB local store at the moment the game enters the
   function (gdb, a few entries);
2. **replay** each captured call thousands of times through the lifted code, restoring both
   before every call, and report the minimum and mean microseconds per call;
3. **compare variants**: every run prints an FNV hash of the final registers and local store, so
   a changed helper or lifter option must give the same hash *and* a better time.

Channels get fixed answers (MFC tag status: all complete; channel counts: 1; writes ignored).
DMA therefore moves no data. What is timed is the lifted code on the captured local store,
which is what helper and lifter changes affect. `SPU_FLOAT_RZ`'s MXCSR mode is on by default,
as in the game; set `JB_EXACT=1` for the exact float forms.

## Use

```sh
# 1. a running instance where the function is hot, e.g. a race
INST=jb work/run.sh >/dev/null 2>&1 &
INST=jb work/tools/to_race.sh
echo "0x0200 100000" > work/inst/jb/pad.txt                   # hold R2

# 2. capture 4 entries (to work/inst/jobbench/<symbol>/: game memory, gitignored)
work/tools/jobbench/capture.sh inst:jb jobmod_67757f5f2148_e2490_spu_func_00004F80 4
# stop the instance with SIGKILL afterwards

# 3. build against the lifted module and replay (pin to one core, keep the box quiet)
work/tools/jobbench/build.sh work/port/spu/jobmod_67757f5f2148_e2490 4F80 /tmp/jb
taskset -c 30 /tmp/jb work/inst/jobbench/jobmod_67757f5f2148_e2490_spu_func_00004F80 0 1 2 3

# a variant: a modified spu_helpers.h in /tmp/variant/ takes precedence
work/tools/jobbench/build.sh work/port/spu/jobmod_67757f5f2148_e2490 4F80 /tmp/jb_v -I/tmp/variant
```

Output per state: `min ... us mean ... us per call (end pc, ret, unresolved) hash`. Use the
minimum. `end pc == ret` means the call returned normally. A nonzero `unresolved` means an
indirect branch went to a function this module doesn't define (another image's code); its time
is missing from the measurement.

`capture.sh` takes `bench.sh`'s lock (`work/inst/.bench.lock`), because gdb stops the game at
every hit. Starting the instance itself should also wait until no bench round is running.

## Reference numbers (2026-11-05, this machine)

`jobmod_67757f5f` `func_4F80`, the job main waits for behind its job-done flag in pack racing,
4 captured states, minimum µs per call:

| float path | state 0 | 1 | 2 | 3 |
|---|---|---|---|---|
| exact (`JB_EXACT=1`) | 80 | 175 | 194 | 148 |
| `SPU_FLOAT_RZ` (default) | 27 | 56 | 66 | 48 |

A `spu_shufb` without per-vector byte swaps (2-6x faster in isolation) and leaner RZ float forms
both measured unchanged here (27.0 vs 27.0 µs) and were not kept. This function is bound by its
fma dependency chains and register pressure, not by helper instruction counts.
