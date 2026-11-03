/* Core definitions shared by the generated PPU code and the runtime.
 *
 * Guest memory is a flat 4 GiB reservation (g_mem); a guest effective
 * address maps to g_mem + (uint32_t)ea. The PS3 is big-endian, so every
 * access byte-swaps.
 *
 * Vector registers are kept byte-reversed: element i (IBM/big-endian
 * numbering) of an N-element vector lives at native index N-1-i. That way
 * every element size reads correctly through the same union.
 */
#pragma once
#include <stdint.h>
#include <string.h>
#include <math.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef union v128 {
    uint8_t u8[16];
    int8_t s8[16];
    uint16_t u16[8];
    int16_t s16[8];
    uint32_t u32[4];
    int32_t s32[4];
    float f32[4];
    uint64_t u64[2];
} v128;

typedef struct PPUContext {
    uint64_t r[32];
    double f[32];
    v128 v[32];
    uint64_t lr, ctr;
    uint8_t cr[8]; /* each field: LT=8 GT=4 EQ=2 SO=1 */
    uint8_t xer_so, xer_ov, xer_ca, xer_cnt;
    uint32_t fpscr, vscr, vrsave;
    uint32_t jt;   /* target of an indirect tail jump (see ppu_dispatch) */
    uint64_t reserve_addr, reserve_val;
    uint32_t reserve_seq;   /* line commit count at lwarx/ldarx (see line_commit) */
    uint32_t thread_id;
    void *host;    /* runtime-owned per-thread state */
} PPUContext;

#define PPU_CTX PPUContext *__restrict ctx
typedef void (*ppu_fn)(PPUContext *ctx);

extern uint8_t *g_mem;

/* ---- runtime services called from generated code ---- */
void ppu_dispatch(PPUContext *ctx);                 /* jump to ctx->jt */
void ppu_call(PPUContext *ctx, uint32_t addr);      /* call by guest address */
void ppu_syscall(PPUContext *ctx);
void ppu_trap(PPUContext *ctx, uint32_t addr);
void ppu_unimpl(PPUContext *ctx, uint32_t addr, uint32_t word);
uint64_t ppu_timebase(void);
void ppu_lswi(PPUContext *ctx, int rd, uint32_t ea, int nb);
void ppu_stswi(PPUContext *ctx, int rs, uint32_t ea, int nb);

#define TAIL(f) __attribute__((musttail)) return f(ctx)

/* ---- memory ---- */
#define EA(x) ((uint32_t)(x))
static inline uint8_t ld8(uint64_t ea) { return g_mem[EA(ea)]; }
static inline uint16_t ld16(uint64_t ea) { uint16_t v; memcpy(&v, g_mem + EA(ea), 2); return __builtin_bswap16(v); }
static inline uint32_t ld32(uint64_t ea) { uint32_t v; memcpy(&v, g_mem + EA(ea), 4); return __builtin_bswap32(v); }
static inline uint64_t ld64(uint64_t ea) { uint64_t v; memcpy(&v, g_mem + EA(ea), 8); return __builtin_bswap64(v); }
static inline void st8(uint64_t ea, uint8_t v) { g_mem[EA(ea)] = v; }
static inline void st16(uint64_t ea, uint16_t v) { v = __builtin_bswap16(v); memcpy(g_mem + EA(ea), &v, 2); }
static inline void st32(uint64_t ea, uint32_t v) { v = __builtin_bswap32(v); memcpy(g_mem + EA(ea), &v, 4); }
static inline void st64(uint64_t ea, uint64_t v) { v = __builtin_bswap64(v); memcpy(g_mem + EA(ea), &v, 8); }

static inline v128 ldv(uint64_t ea) { /* 16-byte aligned load, reversed */
    v128 r; const uint8_t *p = g_mem + (EA(ea) & ~15u);
    for (int i = 0; i < 16; i++) r.u8[15 - i] = p[i];
    return r;
}
static inline void stv(uint64_t ea, v128 v) {
    uint8_t *p = g_mem + (EA(ea) & ~15u);
    for (int i = 0; i < 16; i++) p[i] = v.u8[15 - i];
}

/* ---- bit casts ---- */
static inline uint64_t d2u(double d) { uint64_t u; memcpy(&u, &d, 8); return u; }
static inline double u2d(uint64_t u) { double d; memcpy(&d, &u, 8); return d; }
static inline uint32_t f2u(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }
static inline float u2f(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }

/* ---- integer helpers ---- */
static inline uint32_t rotl32(uint32_t x, unsigned n) { n &= 31; return n ? (x << n) | (x >> (32 - n)) : x; }
static inline uint64_t rotl64(uint64_t x, unsigned n) { n &= 63; return n ? (x << n) | (x >> (64 - n)) : x; }
static inline uint64_t dup32(uint64_t x) { x &= 0xffffffffu; return x | (x << 32); }
static inline uint8_t cmp_s(int64_t a, int64_t b) { return a < b ? 8 : a > b ? 4 : 2; }
static inline uint8_t cmp_u(uint64_t a, uint64_t b) { return a < b ? 8 : a > b ? 4 : 2; }
static inline uint8_t cmp_f(double a, double b) { return a < b ? 8 : a > b ? 4 : a == b ? 2 : 1; }
#define CR0(x) (ctx->cr[0] = cmp_s((int64_t)(x), 0) | ctx->xer_so)
#define CRBIT(n) ((ctx->cr[(n) >> 2] >> (3 - ((n) & 3))) & 1)
#define SETCRBIT(n, b) (ctx->cr[(n) >> 2] = (uint8_t)((ctx->cr[(n) >> 2] & ~(1u << (3 - ((n) & 3)))) | ((b) << (3 - ((n) & 3)))))

static inline uint32_t get_cr(PPUContext *ctx) {
    uint32_t v = 0;
    for (int i = 0; i < 8; i++) v |= (uint32_t)(ctx->cr[i] & 15) << (28 - 4 * i);
    return v;
}
static inline void set_crf(PPUContext *ctx, uint32_t crm, uint32_t v) {
    for (int i = 0; i < 8; i++)
        if (crm & (0x80u >> i)) ctx->cr[i] = (v >> (28 - 4 * i)) & 15;
}
static inline uint64_t get_xer(PPUContext *ctx) {
    return ((uint64_t)ctx->xer_so << 31) | ((uint64_t)ctx->xer_ov << 30) | ((uint64_t)ctx->xer_ca << 29) | ctx->xer_cnt;
}
static inline void set_xer(PPUContext *ctx, uint64_t v) {
    ctx->xer_so = (v >> 31) & 1; ctx->xer_ov = (v >> 30) & 1; ctx->xer_ca = (v >> 29) & 1; ctx->xer_cnt = v & 0x7f;
}

/* add with carry out (64-bit mode: CA is the carry out of bit 0) */
static inline uint64_t adde64(uint64_t a, uint64_t b, uint8_t cin, uint8_t *cout) {
    uint64_t s = a + b, r = s + cin;
    *cout = (s < a) | (r < s);
    return r;
}

static inline uint64_t sraw_(PPUContext *ctx, uint64_t rs, uint64_t rb) {
    int32_t x = (int32_t)rs; unsigned n = rb & 0x3f;
    if (n > 31) { ctx->xer_ca = x < 0; return (uint64_t)(int64_t)(x < 0 ? -1 : 0); }
    ctx->xer_ca = x < 0 && n && ((uint32_t)x & ((1u << n) - 1));
    return (uint64_t)(int64_t)(x >> n);
}
static inline uint64_t srad_(PPUContext *ctx, uint64_t rs, uint64_t rb) {
    int64_t x = (int64_t)rs; unsigned n = rb & 0x7f;
    if (n > 63) { ctx->xer_ca = x < 0; return (uint64_t)(x < 0 ? -1 : 0); }
    ctx->xer_ca = x < 0 && n && ((uint64_t)x & ((1ull << n) - 1));
    return (uint64_t)(x >> n);
}
static inline uint64_t mulhd_(int64_t a, int64_t b) { return (uint64_t)(((__int128)a * b) >> 64); }
static inline uint64_t mulhdu_(uint64_t a, uint64_t b) { return (uint64_t)(((unsigned __int128)a * b) >> 64); }
static inline uint64_t divw_(uint64_t a, uint64_t b) {
    int32_t x = (int32_t)a, y = (int32_t)b;
    if (y == 0 || (x == INT32_MIN && y == -1)) return 0;
    return (uint32_t)(x / y);
}
static inline uint64_t divwu_(uint64_t a, uint64_t b) { return (uint32_t)b ? (uint32_t)a / (uint32_t)b : 0; }
static inline uint64_t divd_(uint64_t a, uint64_t b) {
    int64_t x = (int64_t)a, y = (int64_t)b;
    if (y == 0 || (x == INT64_MIN && y == -1)) return 0;
    return (uint64_t)(x / y);
}
static inline uint64_t divdu_(uint64_t a, uint64_t b) { return b ? a / b : 0; }
static inline uint64_t cntlzw_(uint64_t x) { return (uint32_t)x ? __builtin_clz((uint32_t)x) : 32; }
static inline uint64_t cntlzd_(uint64_t x) { return x ? __builtin_clzll(x) : 64; }

/* ---- atomics (lwarx/stwcx. reservation) ----
 * Conditional stores take a spinlock per 128-byte line, shared with SPU
 * PUTLLC, so a lock-line commit (several 64-bit words) is atomic against
 * them, as on hardware.
 *
 * A reservation is lost when anyone else commits to the line, not only when
 * the word's value differs: every atomic commit (stwcx./stdcx., SPU
 * PUTLLC/PUTLLUC, host LineGuard updates) bumps the line's commit count, and
 * a conditional store fails if the count moved since lwarx. Comparing values
 * alone let a pop from a lock-free list succeed after other threads popped
 * and pushed the same head back (ABA), handing one item to two owners. */
#define LINE_LOCKS 4096
extern uint32_t g_line_locks[LINE_LOCKS];
extern uint32_t g_line_seq[LINE_LOCKS];
/* Lines whose commits wake someone (SPURS ready counts: spurs_kernel.cpp line_notify).
   Checked after every atomic commit's unlock: one byte load. */
extern uint8_t g_line_notify[LINE_LOCKS];
void line_notify(uint64_t ea);
/* Block until line ea (128 bytes, currently equal to `seen`) gets an atomic commit, or
   timeout_us passes (memory.cpp). Commits wake waiters through line_notify. */
void line_wait(uint64_t ea, const void *seen, uint32_t timeout_us);
void line_wake(uint64_t ea);
static inline uint32_t line_index(uint64_t ea) { return (uint32_t)(EA(ea) >> 7) & (LINE_LOCKS - 1); }
static inline void line_lock(uint64_t ea) {
    uint32_t *l = &g_line_locks[line_index(ea)];
    while (__atomic_exchange_n(l, 1, __ATOMIC_ACQUIRE))
        while (__atomic_load_n(l, __ATOMIC_RELAXED)) __builtin_ia32_pause();
}
static inline void line_unlock(uint64_t ea) {
    __atomic_store_n(&g_line_locks[line_index(ea)], 0, __ATOMIC_RELEASE);
    if (__builtin_expect(g_line_notify[line_index(ea)], 0)) line_notify(ea);
}
/* Under line_lock: record a commit to the line, breaking others' reservations. */
static inline void line_commit(uint64_t ea) {
    __atomic_store_n(&g_line_seq[line_index(ea)], g_line_seq[line_index(ea)] + 1, __ATOMIC_RELEASE);
}
static inline uint32_t line_seq(uint64_t ea) { return __atomic_load_n(&g_line_seq[line_index(ea)], __ATOMIC_ACQUIRE); }
static inline uint64_t lwarx_(PPUContext *ctx, uint64_t ea) {
    ctx->reserve_addr = EA(ea);
    ctx->reserve_seq = line_seq(ea);
    ctx->reserve_val = __builtin_bswap32(__atomic_load_n((uint32_t *)(g_mem + EA(ea)), __ATOMIC_SEQ_CST));
    return ctx->reserve_val;
}
static inline uint64_t ldarx_(PPUContext *ctx, uint64_t ea) {
    ctx->reserve_addr = EA(ea);
    ctx->reserve_seq = line_seq(ea);
    ctx->reserve_val = __builtin_bswap64(__atomic_load_n((uint64_t *)(g_mem + EA(ea)), __ATOMIC_SEQ_CST));
    return ctx->reserve_val;
}
static inline int stwcx_(PPUContext *ctx, uint64_t ea, uint64_t v) {
    if (ctx->reserve_addr != EA(ea)) return 0;
    ctx->reserve_addr = ~0ull;
    uint32_t expect = __builtin_bswap32((uint32_t)ctx->reserve_val);
    line_lock(ea);
    int ok = g_line_seq[line_index(ea)] == ctx->reserve_seq &&
             __atomic_compare_exchange_n((uint32_t *)(g_mem + EA(ea)), &expect, __builtin_bswap32((uint32_t)v), 0,
                                         __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    if (ok) line_commit(ea);
    line_unlock(ea);
    return ok;
}
static inline int stdcx_(PPUContext *ctx, uint64_t ea, uint64_t v) {
    if (ctx->reserve_addr != EA(ea)) return 0;
    ctx->reserve_addr = ~0ull;
    uint64_t expect = __builtin_bswap64(ctx->reserve_val);
    line_lock(ea);
    int ok = g_line_seq[line_index(ea)] == ctx->reserve_seq &&
             __atomic_compare_exchange_n((uint64_t *)(g_mem + EA(ea)), &expect, __builtin_bswap64(v), 0,
                                         __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    if (ok) line_commit(ea);
    line_unlock(ea);
    return ok;
}

/* ---- floating point ---- */
static inline double rnd_s(double x) { return (double)(float)x; }
static inline uint64_t fctiwz_(double x) {
    int32_t i = x != x ? INT32_MIN : x >= 2147483647.0 ? INT32_MAX : x <= -2147483648.0 ? INT32_MIN : (int32_t)x;
    return (uint32_t)i;
}
static inline uint64_t fctiw_(double x) { return fctiwz_(nearbyint(x)); }
static inline uint64_t fctidz_(double x) {
    int64_t i = x != x ? INT64_MIN : x >= 9223372036854775807.0 ? INT64_MAX : x <= -9223372036854775808.0 ? INT64_MIN : (int64_t)x;
    return (uint64_t)i;
}
static inline uint64_t fctid_(double x) { return fctidz_(nearbyint(x)); }
static inline double fsel_(double a, double b, double c) { return a >= 0.0 ? b : c; }

#include "vmx.h"

#ifdef __cplusplus
}
#endif
