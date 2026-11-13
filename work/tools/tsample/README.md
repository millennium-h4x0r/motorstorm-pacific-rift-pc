# tsample: per-thread CPU sampler

Linux only (perf_event_open, /proc). On macOS use the system's sampler,
`/usr/bin/sample <pid> 4 1 -file out.txt`, which gives every thread's call
tree with symbols.

Samples one thread's user-space instruction pointer, with the frame-pointer
callchain where there is one, through perf_event_open's task clock. No root
and no `perf` binary needed: it works with perf_event_paranoid=2 on any
process you could attach gdb to.

    cc -O2 -o work/tools/tsample/tsample work/tools/tsample/tsample.c
    work/tools/tsample/tsample <tid> <seconds> [hz=1000] > samples.txt
    python3 work/tools/tsample/tsym.py <pid> samples.txt     # leaf and inclusive histograms

The game's main thread is the process id (tid == pid). Other threads: `ls /proc/<pid>/task`,
names in `/proc/<pid>/task/<tid>/comm`. Only on-CPU time is sampled, so a sleeping
thread shows as fewer samples, not as a sleep frame. Lifted code has no frame
pointers, so inclusive numbers are mostly leaf numbers.
