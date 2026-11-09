/* MotorStorm: Pacific Rift port -- the functions that work/tools/patch_lift.py
 * splices calls to into the lifted PPU code. Each runs at the entry of one
 * lifted game function, before its body, and may adjust the arguments. */
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>

/* The lifted code's register file; gpr[32] is its first member (see
 * ppu_recomp.h / runtime/ppu/ppu_context.h). Same tag, so the same symbol. */
struct ppu_context;
static inline uint64_t& gpr(ppu_context* c, int i) { return reinterpret_cast<uint64_t*>(c)[i]; }

/* PS3RECOMP_FPS=60: run the game at 60 fps at the right speed.
 *
 * The game's frame timer has a fixed step of n * 1e6 / 60 us, where n is the
 * number of vblanks per frame passed to its SetInterval (func_00435D18), and
 * its vblank handler releases the RSX's pre-flip semaphore every n vblanks
 * (the threshold is copied from the same field each frame). The game asks for
 * n = 2: a 1/30 s step and a flip every second vblank. With n = 1 the step is
 * 1/60 s and a frame may flip on every vblank, so the simulation advances at
 * the same speed per second at twice the frame rate (measured: the game clock
 * gains exactly 1/60 per flip). Default 30, the game as shipped. */
void port_timer_interval(ppu_context* ctx)
{
    static int fps = -1;
    if (fps < 0) {
        const char* e = getenv("PS3RECOMP_FPS");
        fps = (e && atoi(e) == 60) ? 60 : 30;
        if (fps == 60) printf("[fps] 60 fps mode: frame timer interval 2 -> 1 vblank\n");
    }
    if (fps == 60 && (uint32_t)gpr(ctx, 4) == 2) gpr(ctx, 4) = 1;
}

/* PS3RECOMP_FPS=60: a frame that misses 60 still advances the game by the
 * time it took. func_008CF1C0 runs once a frame with the frame step in f1
 * (1/60 in 60 fps mode, scaled by the game itself, e.g. 1/20 of it in the
 * crash camera's slow motion) and hands it to the game clock and the other
 * per-frame updates. A frame shown for two vblanks gets twice the step, the
 * 30 fps mode's step, instead of playing in slow motion. At most
 * PS3RECOMP_DT_MAX vblanks (default 2, the step the game ships with) are
 * made up; PS3RECOMP_DT_MAX=1 turns this off. PS3RECOMP_DT_LOG=1 logs it. */
extern "C" uint32_t cellGcm_vblank_ticks(void);
static inline double& fpr(ppu_context* c, int i) { return reinterpret_cast<double*>(reinterpret_cast<uint64_t*>(c) + 32)[i]; }
void port_frame_dt(ppu_context* ctx)
{
    static int fps = -1, maxv = 2, log = 0, force = 0;
    if (fps < 0) {
        const char* e = getenv("PS3RECOMP_FPS");
        fps = (e && atoi(e) == 60) ? 60 : 30;
        e = getenv("PS3RECOMP_DT_MAX"); if (e && atoi(e) >= 1) maxv = atoi(e);
        e = getenv("PS3RECOMP_DT_LOG"); log = e && *e == '1';
        e = getenv("PS3RECOMP_DT_FORCE"); force = e ? atoi(e) : 0;   /* test: always this many */
    }
    static uint32_t last;
    const uint32_t v = cellGcm_vblank_ticks();
    uint32_t d = last ? v - last : 1u;
    last = v;
    if (fps != 60) return;
    if (force > 0) d = (uint32_t)force;
    if (d < 1u) d = 1u;
    if (d > (uint32_t)maxv) d = (uint32_t)maxv;
    const double f1 = fpr(ctx, 1);
    if (log) {
        static double lf; static uint32_t n, made;
        n++; if (d > 1u) made++;
        if (f1 != lf || (n % 600) == 0) {
            struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
            fprintf(stderr, "[dt] t=%.3f frame %u f1=%.6f x%u (stretched %u)\n",
                    ts.tv_sec + ts.tv_nsec / 1e9, n, f1, d, made);
            lf = f1;
        }
    }
    if (d > 1u) fpr(ctx, 1) = f1 * (double)d;
}
