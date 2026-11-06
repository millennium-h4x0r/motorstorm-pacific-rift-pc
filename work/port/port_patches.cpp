/* MotorStorm: Pacific Rift port -- the functions that work/tools/patch_lift.py
 * splices calls to into the lifted PPU code. Each runs at the entry of one
 * lifted game function, before its body, and may adjust the arguments. */
#include <cstdint>
#include <cstdio>
#include <cstdlib>

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
