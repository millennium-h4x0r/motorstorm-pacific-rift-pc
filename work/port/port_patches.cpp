/* MotorStorm: Pacific Rift port -- the functions that work/tools/patch_lift.py
 * splices calls to into the lifted PPU code. Each runs at the entry of one
 * lifted game function, before its body, and may adjust the arguments. */
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <ctime>
#include "ppu_context.h"

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
 * PS3RECOMP_DT_MAX vblanks are made up. Default 1 (off): the made-up step
 * lands a frame late (the long frame moved one step, the next one jumps two),
 * so below 60 the car lurched back and forth along the road and the physics
 * alternated 1/60 and 1/30 steps (riders thrown off). Off, every frame is
 * 1/60 s and the game runs a little slow while it is below 60 fps;
 * PS3RECOMP_DT_MAX=2 restores the catch-up. PS3RECOMP_DT_LOG=1 logs it. */
extern "C" uint32_t cellGcm_vblank_ticks(void);
static inline double& fpr(ppu_context* c, int i) { return reinterpret_cast<double*>(reinterpret_cast<uint64_t*>(c) + 32)[i]; }
void port_frame_dt(ppu_context* ctx)
{
    static int fps = -1, maxv = 1, log = 0, force = 0;
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

extern "C" uint8_t* vm_base;
static inline uint32_t be32(uint32_t a) { const uint8_t* p = vm_base + a; return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static inline void wbe32(uint32_t a, uint32_t v) { uint8_t* p = vm_base + a; p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }

/* Camera shake, off by default (both frame rates); PS3RECOMP_CAMERA_SHAKE=1
 * keeps the game's own. Two systems shake the picture: the chase camera's
 * shake springs (port_shake_skip below) and evo::camerasystem::Shake, the
 * jolts on bumps, landings and impacts (port_camshake_*). With it on, the
 * 60 fps fixes below keep both at their 30 Hz behaviour. */
static int camera_shake_on(void)
{
    static int on = -1;
    if (on < 0) { const char* e = getenv("PS3RECOMP_CAMERA_SHAKE"); on = e && *e == '1'; }
    return on;
}

/* PS3RECOMP_FPS=60: the chase camera's shake at the rate it was tuned for.
 * func_0037FB68(camera r3, vehicle r4) steps the shake springs
 * (CHASE_SHAKE_* in data/camera_config_files/chase_cameras/: per-step max
 * acceleration and damping) once per call and never reads the frame time.
 * At 60 fps it took twice as many steps a second, which shook the picture
 * much harder over rough ground. It now runs once per two vblanks per camera,
 * the 30 fps cadence, and keeps its last state in between.
 * PS3RECOMP_SHAKE_60=1 turns this off. Returns nonzero to skip the call.
 * Camera shake off: zeroes the shake amounts instead, every call (the camera
 * reloads them when the vehicle changes). */
int port_shake_skip(ppu_context* ctx)
{
    if (!camera_shake_on()) {
        /* CHASE_SHAKE_SLOW_AMOUNT (+0x230) and _FAST_AMOUNT (+0x260), x y z:
         * zero, so the springs settle at rest (verified against the
         * buggy00 config: 2 2 1 and 7 20 5 at these offsets). */
        const uint32_t o = (uint32_t)gpr(ctx, 3);
        for (uint32_t i = 0; i < 12; i += 4) { wbe32(o + 0x230u + i, 0u); wbe32(o + 0x260u + i, 0u); }
        return 0;
    }
    static int on = -1;
    if (on < 0) {
        const char* e = getenv("PS3RECOMP_FPS");
        const char* o = getenv("PS3RECOMP_SHAKE_60");
        on = (e && atoi(e) == 60) && !(o && *o == '1');
    }
    if (!on) return 0;
    static uint32_t cam[8], last[8];
    const uint32_t obj = (uint32_t)gpr(ctx, 3);
    const uint32_t tick = cellGcm_vblank_ticks() / 2u;
    int k = 0;
    while (k < 8 && cam[k] && cam[k] != obj) k++;
    if (k == 8) k = (int)(obj >> 4) & 7;          /* full: reuse a slot */
    static int log = -1; static uint32_t calls, runs;
    if (log < 0) { const char* e = getenv("PS3RECOMP_DT_LOG"); log = e && *e == '1'; }
    if (log && (++calls % 600) == 0)
        fprintf(stderr, "[shake] %u calls, %u stepped\n", calls, runs);
    if (cam[k] == obj && last[k] == tick) return 1;
    cam[k] = obj; last[k] = tick; runs++;
    return 0;
}

/* PS3RECOMP_FPS=60: camera-system shake noise at its 30 Hz rate.
 * evo::camerasystem::Shake's update (func_0042E3F0, vtable 0x0103EB48 slot 2)
 * draws fresh random offsets from the game's RNG (func_00439820: an 8-byte
 * TEA state at *(TOC+0x7448)) on every call, and applies them to the camera
 * matrix it is given. At 60 fps the jolts after a bump or landing (the ones
 * that also rumble) changed direction twice as often. The update still runs
 * every frame, but within one 30 Hz tick a shake replays the draws it made at
 * the tick's first update: each draw is recomputed from the RNG state recorded
 * then, on a scratch copy, so the game's RNG itself is untouched.
 * PS3RECOMP_SHAKE_60=1 turns this off too. */
uint32_t g_shk_rec_total, g_shk_rep_total;
namespace {
struct ShakeRec { uint32_t obj, tick; uint32_t n; uint32_t st[16][2]; };
ShakeRec g_shk[8];
ShakeRec* g_shk_cur;      /* the shake whose update is running, or null */
int g_shk_replay;         /* that update replays its tick's draws */
int g_shk_off;            /* camera shake off: every draw of the update is neutral */
uint32_t g_shk_i;         /* the next draw within it */
}
static int shake_60_on(void)
{
    static int on = -1;
    if (on < 0) {
        const char* e = getenv("PS3RECOMP_FPS");
        const char* o = getenv("PS3RECOMP_SHAKE_60");
        on = (e && atoi(e) == 60) && !(o && *o == '1');
    }
    return on;
}
void port_camshake_enter(ppu_context* ctx)
{
    g_shk_cur = nullptr;
    g_shk_off = !camera_shake_on();
    if (g_shk_off || !shake_60_on()) return;
    const uint32_t obj = (uint32_t)gpr(ctx, 3), tick = cellGcm_vblank_ticks() / 2u;
    int k = 0;
    while (k < 8 && g_shk[k].obj && g_shk[k].obj != obj) k++;
    if (k == 8) k = (int)(obj >> 4) & 7;
    ShakeRec* r = &g_shk[k];
    if (r->obj == obj && r->tick == tick) { g_shk_replay = 1; }
    else { r->obj = obj; r->tick = tick; r->n = 0; g_shk_replay = 0; }
    g_shk_cur = r; g_shk_i = 0;
    static int log = -1; static uint32_t calls, replays; extern uint32_t g_shk_rec_total, g_shk_rep_total;
    if (log < 0) { const char* e = getenv("PS3RECOMP_DT_LOG"); log = e && *e == '1'; }
    calls++; replays += g_shk_replay;
    if (log && (calls % 300) == 0)
        fprintf(stderr, "[camshake] %u updates, %u replayed, draws recorded %u replayed %u\n", calls, replays, g_shk_rec_total, g_shk_rep_total);
}
/* At func_00439820's entry. Nonzero: f1 holds the draw, skip the body. */
int port_camshake_rand(ppu_context* ctx)
{
    const uint32_t lr = (uint32_t)ctx->lr;
    if (lr < 0x0042E3F0u || lr >= 0x0042F6E8u) return 0;          /* not a draw of the shake */
    if (g_shk_off) {
        /* The shake turns each draw r into 2r - c, c the float at
         * TOC+0x7010, times its amplitude: r = c/2 is no offset. The real
         * draw still runs (the wrapper's body), so the game's RNG sequence
         * is unchanged. */
        const uint64_t lr0 = ctx->lr;
        gpr(ctx, 3) = be32((uint32_t)gpr(ctx, 2) + 0x7448u);
        ctx->lr = 0x00439834u;
        { void func_00439678(ppu_context*); func_00439678(ctx); }
        ctx->lr = lr0;
        const uint32_t cu = be32((uint32_t)gpr(ctx, 2) + 0x7010u);
        float c; memcpy(&c, &cu, 4);
        fpr(ctx, 1) = (double)c * 0.5;
        return 1;
    }
    ShakeRec* r = g_shk_cur;
    if (!r) return 0;
    const uint32_t rng = be32((uint32_t)gpr(ctx, 2) + 0x7448u);
    if (!g_shk_replay) {
        if (r->n < 16) { r->st[r->n][0] = be32(rng); r->st[r->n][1] = be32(rng + 4); r->n++; g_shk_rec_total++; }
        return 0;
    }
    if (g_shk_i >= r->n) return 0;
    /* the recorded state on a scratch copy below the stack pointer */
    const uint32_t scratch = ((uint32_t)gpr(ctx, 1) - 0x200u) & ~15u;
    wbe32(scratch, r->st[g_shk_i][0]); wbe32(scratch + 4, r->st[g_shk_i][1]);
    g_shk_i++; g_shk_rep_total++;
    const uint64_t r3 = gpr(ctx, 3);
    gpr(ctx, 3) = scratch;
    { void func_00439678(ppu_context*); func_00439678(ctx); }   /* leaf: f1 = the draw */
    gpr(ctx, 3) = r3;
    return 1;
}
