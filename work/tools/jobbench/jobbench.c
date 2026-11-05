/* jobbench: replay captured calls of one lifted SPU function, time them, and hash the result.
 *
 * Each capture (capture.sh) is the spu_context and the 256 KB local store at the moment the game
 * entered the function. Every iteration restores both, calls the lifted entry and drains its tail
 * transfers until it returns to the caller (the link register at entry), so the work timed is
 * exactly one real call. Channels get fixed answers (MFC tag status: all complete; channel
 * counts: 1; writes ignored), so DMA does not move data: what is measured is the lifted code on
 * the captured local store. The FNV hash of the final registers and local store (first
 * iteration) makes every variant comparable bit for bit.
 *
 * Built by build.sh against one lifted module (work/port/spu/<module>/spu_recomp.c):
 *   REGFN    <prefix>_spu_recomp_register     ENTRYFN  <prefix>_spu_func_<ENTRY>
 *   ENTRY_PC 0x<ENTRY>
 * Run: jobbench <capdir> <state>... (states are the N in ctx_N.bin / ls_N.bin)
 *   JB_N=<n>     iterations per state (default 2000)
 *   JB_EXACT=1   run the exact float forms instead of SPU_FLOAT_RZ's native ones
 * Prints, per state, the minimum and mean microseconds per call. Use the minimum: it is the least
 * disturbed by other load. Pin it to one core (taskset -c N) and keep the machine quiet. */
#include "spu_context.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <xmmintrin.h>

extern __thread int g_spu_float_rz;   /* spu_helpers.h (weak); set with MXCSR, as spu_float_mode_enter */

/* ---- the runtime surface the lifted code links against, reduced to the replay ---------- */
SPU_THREAD_LOCAL void (*g_spu_trampoline_fn)(spu_context*);
volatile int g_spu_direct = 0;
uint8_t g_spu_stack_reset_hint[1024];
int g_spu_ls_watch_n = 0;
void spu_ls_watch_slow(uint32_t l, int w, const uint8_t* p, uint32_t pc, uint32_t lr)
{ (void)l; (void)w; (void)p; (void)pc; (void)lr; }

#define NFN 16384
static struct { uint32_t a; void (*f)(spu_context*); } s_fn[NFN];
static int s_nfn;
void spu_register_function(uint32_t a, void (*f)(spu_context*))
{ if (s_nfn < NFN) { s_fn[s_nfn].a = a; s_fn[s_nfn++].f = f; } }
static void (*lookup(uint32_t a))(spu_context*)
{ for (int i = 0; i < s_nfn; i++) if (s_fn[i].a == a) return s_fn[i].f; return 0; }

static int unresolved;
void spu_indirect_branch(spu_context* c)
{ c->pc &= SPU_LS_MASK; void (*f)(spu_context*) = lookup(c->pc); if (f) f(c); else unresolved++; }
void spu_begin_image(int id) { (void)id; }
void spu_halt(spu_context* c) { (void)c; fprintf(stderr, "jobbench: halt\n"); exit(3); }
void spu_stop(spu_context* c, uint32_t code) { (void)c; fprintf(stderr, "jobbench: stop 0x%X\n", code); exit(3); }
void spu_drain_call(spu_context* c, uint32_t ret)
{
    while (g_spu_trampoline_fn && (c->pc & SPU_LS_MASK) != ret) {
        void (*f)(spu_context*) = g_spu_trampoline_fn; g_spu_trampoline_fn = 0; f(c);
    }
    g_spu_trampoline_fn = 0;
}
void spu_img_restore(spu_context* c, int32_t id) { (void)c; (void)id; }
void spu_wrch(spu_context* c, uint32_t ch, u128 v) { (void)c; (void)ch; (void)v; }
u128 spu_rdch(spu_context* c, uint32_t ch)
{ (void)c; u128 r; memset(&r, 0, 16); if (ch == 24) r._u32[0] = 0xFFFFFFFFu; return r; }   /* MFC_RdTagStat */
uint32_t spu_rchcnt(spu_context* c, uint32_t ch) { (void)c; (void)ch; return 1; }

extern void REGFN(void);
extern void ENTRYFN(spu_context*);

static double now(void)
{ struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec * 1e-9; }
static void* slurp(const char* p, size_t n)
{
    FILE* f = fopen(p, "rb"); if (!f) { perror(p); exit(2); }
    void* b = aligned_alloc(64, n);
    if (fread(b, 1, n, f) != n) { fprintf(stderr, "jobbench: %s is short\n", p); exit(2); }
    fclose(f); return b;
}
static uint64_t fnv(const void* p, size_t n)
{ const uint8_t* b = p; uint64_t h = 1469598103934665603ULL; for (size_t i = 0; i < n; i++) { h ^= b[i]; h *= 1099511628211ULL; } return h; }

int main(int argc, char** argv)
{
    if (argc < 3) { fprintf(stderr, "usage: %s <capdir> <state>...\n", argv[0]); return 2; }
    REGFN();
    const int N = getenv("JB_N") ? atoi(getenv("JB_N")) : 2000;
    if (!getenv("JB_EXACT")) { _mm_setcsr(0xFFC0u); g_spu_float_rz = 1; }
    for (int s = 2; s < argc; s++) {
        char p[512];
        snprintf(p, sizeof p, "%s/ctx_%s.bin", argv[1], argv[s]); spu_context* base = slurp(p, sizeof(spu_context));
        snprintf(p, sizeof p, "%s/ls_%s.bin", argv[1], argv[s]); uint8_t* ls0 = slurp(p, SPU_LS_SIZE);
        spu_context* c = aligned_alloc(64, sizeof *c);
        uint8_t* ls = aligned_alloc(64, SPU_LS_SIZE);
        const uint32_t ret = base->gpr[0]._u32[0] & SPU_LS_MASK;
        double trun = 0, tmin = 1e9; uint64_t h = 0;
        for (int it = 0; it < N; it++) {
            memcpy(c, base, sizeof *c); memcpy(ls, ls0, SPU_LS_SIZE);
            c->ls = ls; c->host_depth = 1; c->int_enable = 0;
            const double t1 = now();
            c->pc = ENTRY_PC; g_spu_trampoline_fn = 0; ENTRYFN(c); spu_drain_call(c, ret);
            const double t2 = now();
            trun += t2 - t1; if (t2 - t1 < tmin) tmin = t2 - t1;
            if (it == 0) h = fnv(c->gpr, sizeof c->gpr) ^ fnv(ls, SPU_LS_SIZE);
        }
        printf("state %s: min %.2f us mean %.2f us per call (end pc 0x%05X, ret 0x%05X, unresolved %d) hash %016llx\n",
               argv[s], tmin * 1e6, trun / N * 1e6, c->pc & SPU_LS_MASK, ret, unresolved, (unsigned long long)h);
        free(base); free(ls0); free(c); free(ls);
    }
    return 0;
}
