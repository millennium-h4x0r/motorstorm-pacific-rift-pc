/*
 * ps3recomp game project -- entry point template.
 *
 * This is the runner. It boots a lifted title through the toolkit's PPU
 * scaffold: allocate the flat guest VM, load the PPU ELF named on the command
 * line, register the lifted function table and the HLE NID handlers, start a
 * frame clock, and dispatch the entry OPD.
 *
 * The scaffold does the work; nothing below reimplements it. ppu_load_elf,
 * ppu_recomp_register, ppu_hle_init, ppu_sysprx_register, ppu_fs_register,
 * lv2_init_syscalls and ppu_run all come from the toolkit, compiled into this
 * project from PS3RECOMP_DIR (see CMakeLists.txt). What is left here is the
 * part a port owns: which backend to present through, how the frame clock is
 * paced, and whatever diagnostics the title turns out to need.
 *
 * runtime/ppu/tests/boot_main.cpp in the toolkit is the same boot in its
 * fully-instrumented form -- crash filter, hang watchdog, guest-PC sampling
 * profiler. Read it when a boot goes wrong; copy from it what the title
 * needs. It is deliberately not what a fresh project starts with.
 *
 * The one part that IS duplicated from it is the frame clock below, because a
 * port is expected to change its pacing and a shared one would be the wrong
 * shape for that. It was copied faithfully, comments and all. If a pacing bug
 * is fixed in one of the two, look at the other.
 *
 * This builds on Windows, macOS and Linux. CI builds and runs it against the
 * boot smoke title on the last two, and compile-checks it against clang-cl on
 * the first (tools/check_ppu_scaffold.py).
 */

/* The lifter's generated header, which declares ppu_context and the function
 * table. It comes first: everything below is written against that struct. */
#include "ppu_recomp.h"

/* The Win32 names -- Sleep, GetTickCount64, CreateThread, InterlockedIncrement,
 * VirtualAlloc, the scalar typedefs -- from one place. On Windows this is a
 * passthrough to <windows.h>; off Windows the toolkit's shim supplies the same
 * names over pthreads and mmap, so the call sites below are written once. */
#include "win32_compat.h"

#include "settings.h"   /* user/settings.ini, for a windowed run */
#include "ui.h"         /* the in-game menu (F1) and FPS counter (F10) */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>     /* clock_gettime: PS3RECOMP_FLIPLOG */
#include "ps3emu/ps3_ftrace.h"   /* PS3_FTRACE: PRESENT spans */
#include <string>
#ifdef _WIN32
#include <direct.h>   /* _mkdir: the shader cache directory */
#else
#include <sys/stat.h> /* mkdir: the shader cache directory */
#endif

#ifndef _WIN32
#include <sys/mman.h>   /* mmap: the guest arena, see alloc_guest_vm */
#endif

#if defined(__APPLE__)
#include <CoreFoundation/CoreFoundation.h>   /* the main thread's run loop, see run_guest */
#include <pthread.h>
#endif

#ifdef _WIN32
/* timeBeginPeriod: <windows.h> arrives with WIN32_LEAN_AND_MEAN set, which
 * excludes the multimedia timer API, so it is asked for by name -- and after
 * windows.h, since timeapi.h uses UINT. No shim exists off Windows because
 * there is nothing to shim: the 15.6 ms default timer granularity this works
 * around is a Windows problem. */
#include <timeapi.h>
#endif

/* ---------------------------------------------------------------------------
 * What the scaffold gives this file
 * -----------------------------------------------------------------------*/
extern "C" {
uint32_t ppu_load_elf(const char* path);      /* ELF -> guest RAM, returns entry OPD */
void     ppu_recomp_register(void);           /* generated: lifted table -> address map */
void     ppu_hle_init(void);                  /* firmware import NID -> HLE handler */
void     ppu_sysprx_register(void);           /* boot-critical CRT (sys_initialize_tls, ...) */
void     ppu_fs_register(void);               /* cellFs over the real game directory */
void     lv2_init_syscalls(void);             /* the lv2 syscall table */
int      ppu_run(uint32_t entry_opd, uint32_t stack_top);

/* This project's own: load whatever real system PRX modules the title needs
 * into guest RAM and register their exports. stubs.cpp has the empty version
 * a game with no lifted PRX wants. */
void     ps3_load_prx_modules(void);

extern const char* ppu_vfs_root;              /* host dir the PS3 mount points map into */
void     ps3_host_quit(const char* why);      /* cellSysutil.c: REQUEST_EXITGAME, then exit 0 */

/* sys_ppu_thread.c: the main thread's stack, as sys_ppu_thread_get_stack_information
 * reports it. Left at 0 it names VM_STACK_BASE, the first created thread's stack. */
extern uint32_t g_ppu_main_stack_addr;
extern uint32_t g_ppu_main_stack_size;

/* The status the guest handed to sys_process_exit. A title that exits that way
 * never returns through ppu_run, so this is only read on the path where the
 * entry function unwound instead. */
extern int     g_sys_process_exit_called;
extern int32_t g_sys_process_exit_code;

/* Host-provided symbols the runtime and the HLE libraries link against. */
uint8_t* vm_base = nullptr;
extern uint32_t ppu_vm_size;                  /* ppu_loader.cpp: the OOB guard */

/* Guest-callback dispatch. g_ps3_guest_caller is the hook the HLE runtime
 * calls back into recompiled code through -- cellSysutil events and the GCM
 * vblank/flip handlers. ppu_guest_call does the OPD -> dispatch. */
typedef void (*ps3_guest_caller_fn)(uint32_t, uint64_t, uint64_t, uint64_t, uint64_t,
                                    uint64_t, uint64_t, uint64_t, uint64_t);
extern ps3_guest_caller_fn g_ps3_guest_caller;
uint64_t ppu_guest_call(uint32_t, uint64_t, uint64_t, uint64_t, uint64_t,
                        uint64_t, uint64_t, uint64_t, uint64_t);

/* The GCM half of the frame clock. */
void     cellGcmTickVBlank(void);
void     cellGcmTickFlip(void);
int      cellGcm_take_flip_pending(void);
void     cellGcm_rsx_process_fifo(void);      /* drain get -> put */
unsigned cellGcm_flip_request_count(void);
}

/* ---------------------------------------------------------------------------
 * RSX present backend
 * -----------------------------------------------------------------------*/
/* D3D12 on Windows, Metal on Apple, and the null backend's headless software
 * path anywhere else -- the same selection runtime/host/host_posix.c and the
 * toolkit's boot harness make. All three expose the same three entry points,
 * so the frame clock below is backend-agnostic. */
#if defined(_WIN32)
extern "C" int  rsx_d3d12_backend_init(uint32_t w, uint32_t h, const char* title);
extern "C" void rsx_d3d12_backend_present(void);
extern "C" int  rsx_d3d12_backend_pump_messages(void);
#  define rsx_backend_init    rsx_d3d12_backend_init
#  define rsx_backend_present rsx_d3d12_backend_present
#  define rsx_backend_pump    rsx_d3d12_backend_pump_messages
#  define RSX_BACKEND_NAME    "D3D12"
#elif defined(__APPLE__)
extern "C" int  rsx_metal_backend_init(uint32_t w, uint32_t h, const char* title);
extern "C" void rsx_metal_backend_present(void);
extern "C" int  rsx_metal_backend_pump_messages(void);
#  define rsx_backend_init    rsx_metal_backend_init
#  define rsx_backend_present rsx_metal_backend_present
#  define rsx_backend_pump    rsx_metal_backend_pump_messages
#  define RSX_BACKEND_NAME    "Metal"
#elif defined(PS3RECOMP_RSX_VULKAN)
/* Linux with the opt-in Vulkan backend (scripts/build.sh turns it on).
 * PS3RECOMP_VK_WINDOW=1 shows a window; PS3RECOMP_VK_DUMP=<file> dumps frames. */
extern "C" int  rsx_vulkan_backend_init(uint32_t w, uint32_t h, const char* title);
extern "C" void rsx_vulkan_backend_present(void);
extern "C" int  rsx_vulkan_backend_pump_messages(void);
#  define rsx_backend_init    rsx_vulkan_backend_init
#  define rsx_backend_present rsx_vulkan_backend_present
#  define rsx_backend_pump    rsx_vulkan_backend_pump_messages
#  define RSX_BACKEND_NAME    "Vulkan"
#else
extern "C" int  rsx_null_backend_init(uint32_t w, uint32_t h, const char* title);
extern "C" void rsx_null_backend_present(void);
extern "C" int  rsx_null_backend_pump_messages(void);
#  define rsx_backend_init    rsx_null_backend_init
#  define rsx_backend_present rsx_null_backend_present
#  define rsx_backend_pump    rsx_null_backend_pump_messages
#  define RSX_BACKEND_NAME    "null (headless software)"
#endif

/* ---------------------------------------------------------------------------
 * Configuration
 * -----------------------------------------------------------------------*/
/* The flat VM must span every region the PS3 memory map uses, not just the
 * game image: sys_ppu_thread_create puts thread stacks at 0xD0000000, so a
 * smaller arena makes every spawned thread's stack access out of bounds. */
#define VM_SIZE       0x100010000ull   /* the full 32-bit guest space + a 64K guard */
#define STACK_TOP     0x0FF00000u      /* main-thread stack, below the 0x10000000 segment */
#define STACK_SIZE    0x00100000u      /* what the main thread is told it has */
#define WINDOW_WIDTH  1280
#define WINDOW_HEIGHT 720

/* ---------------------------------------------------------------------------
 * Frame clock
 * -----------------------------------------------------------------------*/
/* On real hardware the RSX raises a vblank interrupt about 60 times a second
 * and that is what drives the game's frame loop. With no RSX it is synthesized
 * here: a host thread calls cellGcmTickVBlank/TickFlip, which invoke the
 * handlers the guest registered, and drains the GCM FIFO. Without this the
 * title initialises, registers its handlers and then waits forever.
 *
 * The ticks are driven off real elapsed time rather than off how long present()
 * took. A hidden or occluded window makes present block hard, and pacing the
 * ticks behind it paces the whole game behind it. */
static volatile LONG g_frames_presented = 0;

/* Frames handed to the backend at a guest FLIP boundary -- one per frame the
 * guest actually finished. The presents made before the guest's first flip, so
 * a fresh window is not left blank through a long boot, carry no guest frame
 * and are deliberately not counted. */
extern "C" unsigned ppu_boot_frames_presented(void)
{
    return (unsigned)g_frames_presented;
}

/* PS3RECOMP_FLIPLOG=<file>: one line per guest frame presented, the host
 * monotonic time in microseconds when present() returned. work/tools/fliplog.py
 * turns it into frame-interval percentiles (FMV/race pacing). */
static void fliplog_note(void)
{
    static FILE* f = (FILE*)1;
    if (f == (FILE*)1) {
        const char* e = getenv("PS3RECOMP_FLIPLOG");
        f = (e && *e) ? fopen(e, "w") : nullptr;
    }
    if (!f) return;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    fprintf(f, "%llu\n", (unsigned long long)ts.tv_sec * 1000000ull + (unsigned long long)(ts.tv_nsec / 1000));
    fflush(f);
}

static void present_guest_frame(void)
{
    const uint64_t ft0 = PS3_FT_ON() ? ps3_ft_now() : 0;
    rsx_backend_present();
    if (ft0) ps3_ft_span(PS3_FT_PRESENT, 0, 0, 0, ft0, ps3_ft_now());
    fliplog_note();
    /* This thread increments and guest threads read, so interlocked rather
     * than a volatile ++, which on arm64 is neither atomic nor a fence. */
    InterlockedIncrement(&g_frames_presented);
}

static DWORD WINAPI frame_clock(LPVOID)
{
    const char* title = getenv("PS3_TITLE");
    if (!title || !*title) title = "ps3recomp";

    int rsx_ok = (rsx_backend_init(WINDOW_WIDTH, WINDOW_HEIGHT, title) == 0);
    fprintf(stderr, "[rsx] %s backend init %s\n", RSX_BACKEND_NAME,
            rsx_ok ? "OK -- window open" : "FAILED");

    unsigned  last_flip = 0;
    /* Vblank period and loop cadence, in nanoseconds. The tick used to be a
     * whole 16 ms (62.5 Hz) on a millisecond clock with a Sleep(4) loop: a
     * 29.97 fps Bink movie paced on two vblanks then gained a frame every
     * ~0.8 s and showed one for three vblanks (a visible hitch), presents
     * landed on a 4 ms grid, and every RSX fence the guest waited on cost up
     * to 4 ms before the drain below wrote it. Now 59.94 Hz on the monotonic
     * clock, sleeping to the next tick but at most PS3RECOMP_CLOCK_POLL_US
     * (default 1000) so the drain/present keep up.
     * PS3RECOMP_VBLANK_HZ overrides the rate (e.g. 60, 50). */
    const double hz_env = getenv("PS3RECOMP_VBLANK_HZ") ? atof(getenv("PS3RECOMP_VBLANK_HZ")) : 0.0;
    const unsigned long long period_ns = hz_env > 1.0 ? (unsigned long long)(1e9 / hz_env)
                                                      : 1001000000ull / 60ull;   /* 59.94 Hz */
    const char* poll_env = getenv("PS3RECOMP_CLOCK_POLL_US");
    const unsigned long long poll_ns = (poll_env && atoi(poll_env) > 0 ? (unsigned long long)atoi(poll_env) : 1000ull) * 1000ull;
    auto mono_ns = []() -> unsigned long long {
        struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
        return (unsigned long long)ts.tv_sec * 1000000000ull + (unsigned long long)ts.tv_nsec;
    };
    unsigned long long next_tick = mono_ns();

    for (;;) {
        {   /* sleep to the next vblank, or one poll interval, whichever is sooner */
            unsigned long long now0 = mono_ns();
            unsigned long long wake = now0 + poll_ns;
            if (next_tick > now0 && next_tick < wake) wake = next_tick;
            if (wake > now0) {
                struct timespec d = { (time_t)(wake / 1000000000ull), (long)(wake % 1000000000ull) };
                clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &d, NULL);
            }
        }
        unsigned long long now = mono_ns();

        int fired = 0;
        while ((long long)(now - next_tick) >= 0 && fired < 240) {
            cellGcmTickVBlank();
            cellGcmTickFlip();
            /* Present a pending flip BEFORE draining any further. The flip
             * fires at a get==put frame boundary on the guest thread, so the
             * batch held right now is exactly the completed frame; presenting
             * after the drain races the guest's next-frame writes and shows a
             * mixed one. */
            if (rsx_ok && cellGcm_take_flip_pending()) {
                present_guest_frame();
                last_flip = cellGcm_flip_request_count();
            }
            /* Drain the FIFO every tick. This is what writes the RSX sync-fence
             * labels the game's per-frame logic blocks on, so it has to keep
             * advancing at 60 Hz even while present() throttles. */
            if (rsx_ok) cellGcm_rsx_process_fifo();
            next_tick += period_ns;      /* 59.94 Hz */
            fired++;
        }
        if (fired >= 240) next_tick = now;   /* fell too far behind -- resync */

        /* Drain at the outer cadence too, not only on the 16 ms tick: titles
         * fence every render pass on a label the drain writes, and one wave of
         * those at 16 ms apiece paces the guest into single-figure frame rates.
         * The real RSX writes them in microseconds. */
        if (rsx_ok) {
            if (cellGcm_take_flip_pending()) {
                present_guest_frame();
                last_flip = cellGcm_flip_request_count();
            }
            cellGcm_rsx_process_fifo();

            if (rsx_backend_pump() != 0) {
                rsx_ok = 0;              /* window closed */
                /* ...so quit, as the console's Quit Game does, instead of
                 * leaving the game running with no window. */
                ps3_host_quit("window closed");
                continue;
            }
            /* Present on a guest flip. A present on a fixed clock can catch the
             * drain mid-frame and flash a partial one. Before the first flip
             * present freely, so the window is not blank during boot. */
            unsigned fc = cellGcm_flip_request_count();
            if (fc != last_flip) {
                present_guest_frame();
                last_flip = fc;
            } else if (fc == 0) {
                rsx_backend_present();
            }
        }
    }
    return 0;
}

/* ---------------------------------------------------------------------------
 * VFS root
 * -----------------------------------------------------------------------*/
/* Derive the directory holding PS3_GAME from the EBOOT path
 * <root>/PS3_GAME/USRDIR/EBOOT.elf -> <root>. $PS3_VFS_ROOT overrides. */
static char s_vfs_root[1024];

static void derive_vfs_root(const char* eboot)
{
    const char* env = getenv("PS3_VFS_ROOT");
    if (env && *env) { ppu_vfs_root = env; return; }

    strncpy(s_vfs_root, eboot, sizeof s_vfs_root - 1);
    for (char* p = s_vfs_root; *p; p++) if (*p == '\\') *p = '/';
    /* strip EBOOT.elf, USRDIR and PS3_GAME */
    for (int i = 0; i < 3; i++) { char* s = strrchr(s_vfs_root, '/'); if (s) *s = 0; }
    if (!s_vfs_root[0]) strcpy(s_vfs_root, ".");
    ppu_vfs_root = s_vfs_root;
}

/* ---------------------------------------------------------------------------
 * Guest VM
 * -----------------------------------------------------------------------*/
#ifdef _WIN32
/* Demand-paging for the flat VM: reserve the whole 4 GB guest space, which
 * costs no commit, and commit each 64 KB page on first access. Every 32-bit
 * guest offset is then valid, so a garbage guest pointer reads zero instead of
 * killing the process. Faults outside the arena fall through untouched. */
static LONG WINAPI vm_commit_veh(EXCEPTION_POINTERS* ep)
{
    if (ep->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION) {
        ULONG_PTR fault = ep->ExceptionRecord->ExceptionInformation[1];
        uintptr_t base  = (uintptr_t)vm_base;
        if (vm_base && fault >= base && fault < base + VM_SIZE) {
            void* page = (void*)(fault & ~(uintptr_t)0xFFFF);
            if (VirtualAlloc(page, 0x10000, MEM_COMMIT, PAGE_READWRITE))
                return EXCEPTION_CONTINUE_EXECUTION;
        }
    }
    return EXCEPTION_CONTINUE_SEARCH;
}
#endif

static bool alloc_guest_vm(void)
{
#ifdef _WIN32
    AddVectoredExceptionHandler(1, vm_commit_veh);
    vm_base = (uint8_t*)VirtualAlloc(NULL, VM_SIZE, MEM_RESERVE, PAGE_READWRITE);
    ppu_vm_size = 0;              /* the whole space is backed; no OOB guard needed */
#else
    /* No vectored exception handlers off Windows, so the arena is mapped up
     * front and lazily backed by the OS: only pages the title touches cost
     * anything. It spans the whole 32-bit space plus the guard, as on Windows.
     * It used to stop below 0xE0000000, but the raw SPU windows (local store
     * and problem-state registers, runtime/spu/spu_raw.c) live from there up.
     * Every load and store a title made into one was reported as an
     * out-of-bounds access and dropped, so a raw SPU was never loaded or
     * started, and sys_raw_spu_create, which clears the registers through
     * vm_base directly, wrote past the end of the mapping. With the whole
     * space backed, ppu_vm_size 0 also gives the loader's bounds check the
     * one-compare fast path Windows already has.
     *
     * mmap, not calloc: vm_commit() hands vm_base + addr to mprotect, which
     * takes page-aligned addresses only. glibc serves an allocation this big
     * from its own mmap chunk and returns a pointer 16 bytes past the page
     * start, so every commit failed with EINVAL and sys_memory_allocate
     * answered ENOMEM to every call. mmap's base is page-aligned and zeroed. */
    void* p = mmap(NULL, VM_SIZE, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    vm_base = (p == MAP_FAILED) ? nullptr : (uint8_t*)p;
    ppu_vm_size = 0;              /* the whole space is backed; no OOB guard needed */
#endif
    return vm_base != nullptr;
}

/* ---------------------------------------------------------------------------
 * Main
 * -----------------------------------------------------------------------*/
static void harness_guest_caller(uint32_t opd, uint64_t a0, uint64_t a1,
                                 uint64_t a2, uint64_t a3, uint64_t a4,
                                 uint64_t a5, uint64_t a6, uint64_t a7)
{
    ppu_guest_call(opd, a0, a1, a2, a3, a4, a5, a6, a7);
}

/* ---------------------------------------------------------------------------
 * Running the guest
 * -----------------------------------------------------------------------*/
#if defined(__APPLE__)
/* AppKit only runs on the process's main thread. The Metal backend creates
 * its window and pumps its events with dispatch_sync onto the main queue
 * when it is called from any other thread, as it is from the frame clock.
 * With the guest on the main thread nothing ever serviced that queue: the
 * frame clock blocked in its first rsx_backend_init, no vblank or flip was
 * ever delivered and the FIFO never drained. A title that waits for a flip
 * hangs there. So the guest runs on a thread of its own, with the same
 * 256 MB host stack every guest thread gets (sys_ppu_thread.c), and the
 * main thread turns the run loop until ppu_run returns. Headless runs never
 * touch the main queue, which is why CI does not see the difference. */
struct guest_run { uint32_t entry, stack_top; int rc; volatile int done; };

static void* guest_thread(void* arg)
{
    guest_run* g = (guest_run*)arg;
    g->rc = ppu_run(g->entry, g->stack_top);
    __atomic_store_n(&g->done, 1, __ATOMIC_RELEASE);
    CFRunLoopStop(CFRunLoopGetMain());
    return nullptr;
}

static int run_guest(uint32_t entry, uint32_t stack_top)
{
    static guest_run g;
    g.entry = entry; g.stack_top = stack_top; g.rc = 0; g.done = 0;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 256u * 1024u * 1024u);
    pthread_t th;
    int err = pthread_create(&th, &attr, guest_thread, &g);
    pthread_attr_destroy(&attr);
    if (err != 0) {
        fprintf(stderr, "[boot] could not start the guest thread (%d)\n", err);
        return -1;
    }
    /* Short slices: CFRunLoopStop only ends a run that is already going. */
    while (!__atomic_load_n(&g.done, __ATOMIC_ACQUIRE))
        CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.05, false);
    pthread_join(th, nullptr);
    return g.rc;
}
#else
static int run_guest(uint32_t entry, uint32_t stack_top) { return ppu_run(entry, stack_top); }
#endif

int main(int argc, char** argv)
{
    if (argc < 2) {
        printf("usage: %s <PPU ELF>\n", argv[0]);
        return 2;
    }

#ifdef _WIN32
#pragma comment(lib, "winmm.lib")
    /* 1 ms timer resolution. The default granularity is about 15.6 ms, which
     * inflates every shorter wait the title makes and throttles the whole
     * thing. POSIX timers are already fine-grained. */
    timeBeginPeriod(1);
    setvbuf(stdout, NULL, _IONBF, 0);   /* unbuffered: do not lose prints on a kill */
#endif

    printf("=== ps3recomp game runner ===\n");

    /* The MultiStream SPU core copies its DSP plugins (and flashMP3.pic) into
     * local store at addresses it picks at run time and calls them there.
     * Nothing can be lifted for those, so they have to run on the
     * interpreter; without this the toolkit ends the core's task on the first
     * plugin call ("branched into unlifted LS 0x38790 ... ending the job"),
     * the PPU update thread then waits on the core's event flag for good, and
     * cellAudio port 0 -- all menu music and race sound -- stays silent. */
#ifdef _WIN32
    if (!getenv("SPU_INTERP_UNLIFTED")) _putenv_s("SPU_INTERP_UNLIFTED", "1");
#else
    setenv("SPU_INTERP_UNLIFTED", "1", 0);
#endif

    /* Vulkan draw batching on by default. Without it every engine draw is its
     * own vkQueueSubmit + fence wait; a race frame has ~2200 of them and ran
     * at 1-3 fps (FIFO thread parked in vkWaitForFences). VK_DRAW_BATCH=0
     * turns it off. */
#ifndef _WIN32
    setenv("VK_DRAW_BATCH", "1", 0);
    /* The toolkit's per-thread memo of the SPU context -> MFC engine lookup
     * (runtime/spu/spu_channels.c, mfc_for). Off upstream; without it every
     * MFC channel access takes a rwlock and walks the engine list, ~20% of the
     * race's SPU job thread. Race 8.4 -> 10.6 fps with it. */
    setenv("SPU_MFC_LOOKUP_CACHE", "1", 0);
    /* SPU single precision on native SSE ops (runtime/spu/spu_helpers.h,
     * spu_float_mode_enter): SPU code runs with MXCSR at round toward zero,
     * FTZ and DAZ, so fa/fm/fma are one native op plus a range check instead
     * of the exact f64 emulation. Bit-identical (test_spu_float_exact under
     * the mode). Race job CPU per frame ~-19%, race +6-15% fps.
     * SPU_FLOAT_RZ=0 restores the exact forms. */
    setenv("SPU_FLOAT_RZ", "1", 0);
    /* DEVIATION FROM HARDWARE: the game adds its three WWS job lists (Wws_Job,
     * EvoDefault, EvoHigh) with maxContention=1, so each list runs on one SPU
     * and every race job runs serially. The WWS job manager claims jobs
     * atomically and orders them with its own barriers, so a list can be
     * shared; let each run on up to 6 virtual SPUs (libs/spurs/cellSpurs.c).
     * Race 7.9 -> 13.5 fps at 3 (then the cap); with 6 host lanes below, the
     * main race chain (WWS modules de/d0/c9/cb) runs 6-wide.
     * SPURS_MAXCONT_MIN=1 restores the hardware behaviour. */
    setenv("SPURS_MAXCONT_MIN", "6", 0);
    /* DEVIATION FROM HARDWARE: the game's 5-SPU SPURS instance gets 6 host
     * policy-module lanes (SPURS_HOST_SPUS, libs/spurs/cellSpurs.c); the guest
     * still sees nSpus=5, and instances created with fewer than 5 SPUs keep
     * their own count. 6 is the ceiling: the WWS job manager keeps a halfword
     * per SPU at byte 4+2*spuNum of a quadword, so a 7th SPU would overwrite
     * the list's own counter. Race (gpufetch, pack window) 42.7 -> 51.4 fps,
     * main's job wait 9.1 -> 6.0 ms/frame. SPURS_HOST_SPUS=5 restores 5 lanes. */
    setenv("SPURS_HOST_SPUS", "6", 0);
    /* GPU-side vertex fetch (rsx_draw_engine.c, "GPU-side vertex fetch"):
     * the vertex shader reads and decodes the guest's raw vertex and index
     * bytes from a pool instead of the FIFO thread building reference lists
     * and decoding every vertex. Pack racing FIFO CPU ~27 -> ~15 ms/frame,
     * +5-8 fps. Draws it cannot take keep the CPU fetch.
     * PS3RECOMP_ENG_GPUFETCH=0 restores the CPU fetch everywhere. */
    setenv("PS3RECOMP_ENG_GPUFETCH", "1", 0);
    /* SPURS tasks (Havok creates ~20 a race frame from the main thread) run
     * on a persistent pool of big-stack threads instead of a pthread_create
     * each: ~1 ms/frame of main-thread time, and no fell-into-slow-mode
     * rounds in the A/B (runtime/spu/spu_workload.c). SPU_ASYNC_POOL=0 restores
     * a thread per task. */
    setenv("SPU_ASYNC_POOL", "1", 0);
    /* A guest usleep of at most 20 us yields to its deadline instead of a
     * nanosleep, which lasts ~55 us on Linux (timer slack). Main polls a
     * cellSync mutex shared with Havok's SPU tasks with usleep(1) back-offs;
     * its wait there drops ~1.6 -> 0.7 ms/frame (runtime/syscalls/sys_timer.c).
     * PS3_USLEEP_SPIN_MAX=0 turns it off. */
    setenv("PS3_USLEEP_SPIN_MAX", "20", 0);
#endif

    /* Translated guest shaders kept on disk (rsx_shader_spirv.cpp's cache:
     * one file per shader, keyed by stage + the whole HLSL text, which
     * carries every piece of state the translation baked in, and checked
     * against the glslang version and the translator's build). glslang plus
     * the SPIR-V legaliser take 20-150 ms per new shader on the FIFO thread,
     * so the first race showed a hitch for each one; from the second run on
     * they load from <user dir>/shader_cache. PS3RECOMP_SPIRV_CACHE=<dir>
     * picks another directory; PS3RECOMP_SPIRV_CACHE=0 turns it off. */
    {
        const char* sc = getenv("PS3RECOMP_SPIRV_CACHE");
        if (sc && !strcmp(sc, "0")) {
#ifdef _WIN32
            _putenv_s("PS3RECOMP_SPIRV_CACHE", "");
#else
            unsetenv("PS3RECOMP_SPIRV_CACHE");
#endif
        } else if (!sc || !*sc) {
            const char* u = getenv("PS3_USER_DIR");
            std::string dir = std::string(u && *u ? u : "user");
            std::string cache = dir + "/shader_cache";
#ifdef _WIN32
            _mkdir(dir.c_str()); _mkdir(cache.c_str());
            _putenv_s("PS3RECOMP_SPIRV_CACHE", cache.c_str());
#else
            mkdir(dir.c_str(), 0755); mkdir(cache.c_str(), 0755);
            setenv("PS3RECOMP_SPIRV_CACHE", cache.c_str(), 1);
#endif
        }
    }
    /* The NVIDIA driver's own pipeline disk cache stops keeping new entries
     * at its default size limit, which a few builds of this port fill: every
     * run then compiled each race pipeline again on the FIFO thread
     * (libnvidia-gpucomp ~6% of its time in pack racing; 0.7% once the cache
     * may grow). Give it room unless the user set a size. Other drivers
     * ignore these variables. */
    if (!getenv("__GL_SHADER_DISK_CACHE_SIZE")) {
#ifdef _WIN32
        _putenv_s("__GL_SHADER_DISK_CACHE_SIZE", "10737418240");
#else
        setenv("__GL_SHADER_DISK_CACHE_SIZE", "10737418240", 0);
#endif
    }

    if (!alloc_guest_vm()) {
        fprintf(stderr, "ERROR: could not allocate the guest address space\n");
        return 1;
    }

    uint32_t entry = ppu_load_elf(argv[1]);
    if (!entry) {
        fprintf(stderr, "ERROR: could not load %s\n", argv[1]);
        return 1;
    }

    derive_vfs_root(argv[1]);
    printf("[boot] VFS root: %s\n", ppu_vfs_root);

    ppu_recomp_register();   /* lifted function table -> address map */
    ps3_load_prx_modules();  /* this game's lifted system PRX, if it has any */
    ppu_hle_init();          /* firmware import NID -> HLE handlers */
    ppu_sysprx_register();   /* boot-critical CRT */
    ppu_fs_register();       /* cellFs over the game directory */
    lv2_init_syscalls();     /* the lv2 syscall table */

    /* Install the guest-callback hook, then start the frame clock. It no-ops
     * until the title registers its vblank and flip handlers during init. */
    g_ps3_guest_caller = harness_guest_caller;

    /* A windowed run: the player's settings (user/settings.ini) become the
     * environment the backend reads, below anything already set, and the
     * menu hooks into the window. Offscreen runs skip both. */
    { const char* w = getenv("PS3RECOMP_VK_WINDOW");
      if (w && w[0] && w[0] != '0') {
          port_settings* ps = port_settings_get();
          const int had = port_settings_load(ps, port_settings_path());
          port_settings_export(ps);
          printf("[settings] %s %s: fullscreen %d, FPS counter %d\n", had ? "read" : "defaults, no",
                 port_settings_path(), ps->fullscreen, ps->fps_counter);
          port_ui_init();
      } }

    CreateThread(NULL, 4u * 1024 * 1024, frame_clock, NULL, 0, NULL);

    printf("\n[boot] dispatching entry OPD 0x%08X (stack top 0x%08X)\n\n",
           entry, STACK_TOP);

    /* A title that reads its own stack bounds (to find data its parent left
     * below the top, say) gets the stack it really runs on. */
    g_ppu_main_stack_addr = STACK_TOP - STACK_SIZE;
    g_ppu_main_stack_size = STACK_SIZE;

    int rc = run_guest(entry, STACK_TOP);
    printf("\n[boot] ppu_run returned %d (entry function unwound)\n", rc);

    /* A title that called sys_process_exit never reaches this line: that path
     * ends in the host exit(). Getting here means the entry function returned
     * instead, so hand back the status the guest published if it published
     * one. Returning a hardcoded 0 would report success for a run that never
     * got anywhere. */
    return g_sys_process_exit_called ? (int)g_sys_process_exit_code : 0;
}
