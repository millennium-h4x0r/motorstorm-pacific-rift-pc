/* Exact host fast paths for individual lifted SPU functions.
 *
 * The lifter's --wrap-func NAME (ps3recomp/tools/spu_lifter.py) emits the lifted
 * body as NAME_lifted and a NAME that first calls NAME_fast(ctx), defined here.
 * NAME_fast returns 1 when it performed the whole call (it may finish through
 * NAME_lifted) and 0 to decline, in which case NAME_lifted runs as usual. Without
 * --wrap-func in the lift, nothing here is referenced. Every path must leave the
 * local store and every register exactly as the lifted code would.
 *
 * Lifted function names carry the job module's content hash (jobmod_<sha1:12>_e<entry>),
 * so a different image never reaches these; each fast path also checks a hash of its
 * own code bytes in the local store before taking over.
 */
#include "spu_context.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint64_t fp_fnv(const uint8_t* p, size_t n)
{
    uint64_t h = 0xcbf29ce484222325ull;
    for (size_t i = 0; i < n; i++) h = (h ^ p[i]) * 0x100000001b3ull;
    return h;
}

/* ---------------------------------------------------------------------------
 * jobmod e8d07656 func_4310: a 48 KB local-store word copy.
 *
 * In the race this job runs alone while the main thread waits for its batch
 * (func_0094CC80), and ~all of its 300-390 us is the loop at LS 0x4310: per
 * iteration it copies 8 words from r23 to r22 (each a quadword load, rotate,
 * cwd+shufb insert and store), adds 32 to both (all four lanes, `ai`), and
 * repeats until r22 == r24. Every other register it touches is rewritten
 * inside each iteration from r22/r23 and constants.
 *
 * So, when both pointers are word-aligned, the range is a whole number of
 * iterations inside the local store and source and destination do not
 * overlap, the first n-1 iterations are a plain memcpy (the local store is held
 * in SPU byte order). Then r22/r23 are advanced as n-1 `ai`s would and the
 * lifted loop runs the last iteration itself, which leaves every temporary
 * exactly as before, and continues into the function's tail. The job runs this
 * copy about a dozen times per call (two 48 KB buffers, alternately).
 * SPU_FAST_COPY=1 enables it.
 */
void jobmod_e8d076563401_e0F90_spu_func_00004310_lifted(spu_context* ctx) __attribute__((weak));

int jobmod_e8d076563401_e0F90_spu_func_00004310_fast(spu_context* ctx)
{
    static int on = -1;
    if (on < 0) { const char* e = getenv("SPU_FAST_COPY"); on = e && *e == '1'; }
    if (!on || !jobmod_e8d076563401_e0F90_spu_func_00004310_lifted) return 0;
    /* The loop polls for an interrupt at every iteration; with one pending, let it take it there.
     * One that arrives during the copy is taken at the last iteration instead: later, as if the
     * copy had run faster, which is the only difference. */
    if (ctx->int_enable && (ctx->event_status & ctx->event_mask)) return 0;
    const uint32_t d = ctx->gpr[22]._u32[0], s = ctx->gpr[23]._u32[0], e = ctx->gpr[24]._u32[0];
    if ((d | s) & 3 || e <= d || (e - d) & 31) return 0;
    const uint32_t len = e - d, n = len / 32;
    if (n < 2 || d + len > SPU_LS_SIZE || s + len > SPU_LS_SIZE) return 0;
    if (!(s + len <= d || d + len <= s)) return 0;                 /* overlap: the loop's own order */
    if (fp_fnv(ctx->ls + 0x4310, 0x443C - 0x4310) != 0xfdc5560b88a8c3ddull) return 0;
    const uint32_t done = (n - 1) * 32;
    memcpy(ctx->ls + d, ctx->ls + s, done);
    for (int i = 0; i < 4; i++) { ctx->gpr[22]._u32[i] += done; ctx->gpr[23]._u32[i] += done; }
    { static int logged; if (!logged) { logged = 1;
        fprintf(stderr, "[spu-fast] e8d07656 func_4310: LS copy 0x%05X <- 0x%05X, %u bytes (SPU_FAST_COPY)\n", d, s, len); } }
    jobmod_e8d076563401_e0F90_spu_func_00004310_lifted(ctx);       /* last iteration + tail */
    return 1;
}
