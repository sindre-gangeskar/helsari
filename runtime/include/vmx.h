/* AltiVec / VMX operations. Included from ppu.h.
 *
 * Registers are byte-reversed (see ppu.h), so lane-wise operations are plain
 * native loops; operations that move data between lanes use the BE index
 * helpers below (BEW = big-endian word index i -> native index).
 */
#pragma once

#define BEB(i) (15 - (i))
#define BEH(i) (7 - (i))
#define BEW(i) (3 - (i))

#define V_LANES(name, field, n, expr) \
    static inline v128 name(v128 a, v128 b) { v128 r; for (int i = 0; i < n; i++) r.field[i] = (expr); return r; }

static inline int64_t sat_s(int64_t x, int64_t lo, int64_t hi) { return x < lo ? lo : x > hi ? hi : x; }

/* ---- integer add/sub ---- */
V_LANES(vaddubm, u8, 16, (uint8_t)(a.u8[i] + b.u8[i]))
V_LANES(vadduhm, u16, 8, (uint16_t)(a.u16[i] + b.u16[i]))
V_LANES(vadduwm, u32, 4, a.u32[i] + b.u32[i])
V_LANES(vaddcuw, u32, 4, (uint32_t)(((uint64_t)a.u32[i] + b.u32[i]) >> 32))
V_LANES(vaddubs, u8, 16, (uint8_t)sat_s((int)a.u8[i] + b.u8[i], 0, 255))
V_LANES(vadduhs, u16, 8, (uint16_t)sat_s((int)a.u16[i] + b.u16[i], 0, 65535))
V_LANES(vadduws, u32, 4, (uint32_t)sat_s((int64_t)a.u32[i] + b.u32[i], 0, 0xffffffffll))
V_LANES(vaddsbs, s8, 16, (int8_t)sat_s(a.s8[i] + b.s8[i], -128, 127))
V_LANES(vaddshs, s16, 8, (int16_t)sat_s(a.s16[i] + b.s16[i], -32768, 32767))
V_LANES(vaddsws, s32, 4, (int32_t)sat_s((int64_t)a.s32[i] + b.s32[i], INT32_MIN, INT32_MAX))
V_LANES(vsububm, u8, 16, (uint8_t)(a.u8[i] - b.u8[i]))
V_LANES(vsubuhm, u16, 8, (uint16_t)(a.u16[i] - b.u16[i]))
V_LANES(vsubuwm, u32, 4, a.u32[i] - b.u32[i])
V_LANES(vsubcuw, u32, 4, a.u32[i] >= b.u32[i])
V_LANES(vsububs, u8, 16, (uint8_t)sat_s((int)a.u8[i] - b.u8[i], 0, 255))
V_LANES(vsubuhs, u16, 8, (uint16_t)sat_s((int)a.u16[i] - b.u16[i], 0, 65535))
V_LANES(vsubuws, u32, 4, (uint32_t)sat_s((int64_t)a.u32[i] - b.u32[i], 0, 0xffffffffll))
V_LANES(vsubsbs, s8, 16, (int8_t)sat_s(a.s8[i] - b.s8[i], -128, 127))
V_LANES(vsubshs, s16, 8, (int16_t)sat_s(a.s16[i] - b.s16[i], -32768, 32767))
V_LANES(vsubsws, s32, 4, (int32_t)sat_s((int64_t)a.s32[i] - b.s32[i], INT32_MIN, INT32_MAX))

/* ---- max/min/avg ---- */
V_LANES(vmaxub, u8, 16, a.u8[i] > b.u8[i] ? a.u8[i] : b.u8[i])
V_LANES(vmaxuh, u16, 8, a.u16[i] > b.u16[i] ? a.u16[i] : b.u16[i])
V_LANES(vmaxuw, u32, 4, a.u32[i] > b.u32[i] ? a.u32[i] : b.u32[i])
V_LANES(vmaxsb, s8, 16, a.s8[i] > b.s8[i] ? a.s8[i] : b.s8[i])
V_LANES(vmaxsh, s16, 8, a.s16[i] > b.s16[i] ? a.s16[i] : b.s16[i])
V_LANES(vmaxsw, s32, 4, a.s32[i] > b.s32[i] ? a.s32[i] : b.s32[i])
V_LANES(vminub, u8, 16, a.u8[i] < b.u8[i] ? a.u8[i] : b.u8[i])
V_LANES(vminuh, u16, 8, a.u16[i] < b.u16[i] ? a.u16[i] : b.u16[i])
V_LANES(vminuw, u32, 4, a.u32[i] < b.u32[i] ? a.u32[i] : b.u32[i])
V_LANES(vminsb, s8, 16, a.s8[i] < b.s8[i] ? a.s8[i] : b.s8[i])
V_LANES(vminsh, s16, 8, a.s16[i] < b.s16[i] ? a.s16[i] : b.s16[i])
V_LANES(vminsw, s32, 4, a.s32[i] < b.s32[i] ? a.s32[i] : b.s32[i])
V_LANES(vavgub, u8, 16, (uint8_t)(((unsigned)a.u8[i] + b.u8[i] + 1) >> 1))
V_LANES(vavguh, u16, 8, (uint16_t)(((unsigned)a.u16[i] + b.u16[i] + 1) >> 1))
V_LANES(vavguw, u32, 4, (uint32_t)(((uint64_t)a.u32[i] + b.u32[i] + 1) >> 1))
V_LANES(vavgsb, s8, 16, (int8_t)((a.s8[i] + b.s8[i] + 1) >> 1))
V_LANES(vavgsh, s16, 8, (int16_t)((a.s16[i] + b.s16[i] + 1) >> 1))
V_LANES(vavgsw, s32, 4, (int32_t)(((int64_t)a.s32[i] + b.s32[i] + 1) >> 1))

/* ---- rotates / shifts ---- */
V_LANES(vrlb, u8, 16, (uint8_t)((a.u8[i] << (b.u8[i] & 7)) | (a.u8[i] >> ((8 - (b.u8[i] & 7)) & 7))))
V_LANES(vrlh, u16, 8, (uint16_t)((a.u16[i] << (b.u16[i] & 15)) | (a.u16[i] >> ((16 - (b.u16[i] & 15)) & 15))))
V_LANES(vrlw, u32, 4, rotl32(a.u32[i], b.u32[i]))
V_LANES(vslb, u8, 16, (uint8_t)(a.u8[i] << (b.u8[i] & 7)))
V_LANES(vslh, u16, 8, (uint16_t)(a.u16[i] << (b.u16[i] & 15)))
V_LANES(vslw, u32, 4, a.u32[i] << (b.u32[i] & 31))
V_LANES(vsrb, u8, 16, (uint8_t)(a.u8[i] >> (b.u8[i] & 7)))
V_LANES(vsrh, u16, 8, (uint16_t)(a.u16[i] >> (b.u16[i] & 15)))
V_LANES(vsrw, u32, 4, a.u32[i] >> (b.u32[i] & 31))
V_LANES(vsrab, s8, 16, (int8_t)(a.s8[i] >> (b.u8[i] & 7)))
V_LANES(vsrah, s16, 8, (int16_t)(a.s16[i] >> (b.u16[i] & 15)))
V_LANES(vsraw, s32, 4, a.s32[i] >> (b.u32[i] & 31))

/* whole-register shifts; native u64[1] holds the BE high half */
static inline v128 vsl(v128 a, v128 b) {
    unsigned n = b.u8[0] & 7; v128 r;
    r.u64[1] = n ? (a.u64[1] << n) | (a.u64[0] >> (64 - n)) : a.u64[1];
    r.u64[0] = a.u64[0] << n;
    return r;
}
static inline v128 vsr(v128 a, v128 b) {
    unsigned n = b.u8[0] & 7; v128 r;
    r.u64[0] = n ? (a.u64[0] >> n) | (a.u64[1] << (64 - n)) : a.u64[0];
    r.u64[1] = a.u64[1] >> n;
    return r;
}
static inline v128 vslo(v128 a, v128 b) {
    unsigned n = (b.u8[0] >> 3) & 15; v128 r;
    for (int i = 0; i < 16; i++) r.u8[BEB(i)] = i + n < 16 ? a.u8[BEB(i + n)] : 0;
    return r;
}
static inline v128 vsro(v128 a, v128 b) {
    unsigned n = (b.u8[0] >> 3) & 15; v128 r;
    for (int i = 0; i < 16; i++) r.u8[BEB(i)] = i >= (int)n ? a.u8[BEB(i - n)] : 0;
    return r;
}

/* ---- logical ---- */
V_LANES(vand, u64, 2, a.u64[i] & b.u64[i])
V_LANES(vandc, u64, 2, a.u64[i] & ~b.u64[i])
V_LANES(vor, u64, 2, a.u64[i] | b.u64[i])
V_LANES(vxor, u64, 2, a.u64[i] ^ b.u64[i])
V_LANES(vnor, u64, 2, ~(a.u64[i] | b.u64[i]))
static inline v128 vsel(v128 a, v128 b, v128 c) {
    v128 r; for (int i = 0; i < 2; i++) r.u64[i] = (a.u64[i] & ~c.u64[i]) | (b.u64[i] & c.u64[i]); return r;
}

/* ---- multiplies (even = BE even lanes) ---- */
#define V_MULEO(name, dst, dn, srcf, ty, odd) \
    static inline v128 name(v128 a, v128 b) { v128 r; for (int i = 0; i < dn; i++) { \
        int s = 2 * i + odd; r.dst[(dn - 1) - i] = (ty)a.srcf[(2 * dn - 1) - s] * (ty)b.srcf[(2 * dn - 1) - s]; } return r; }
V_MULEO(vmuleub, u16, 8, u8, uint16_t, 0)
V_MULEO(vmuloub, u16, 8, u8, uint16_t, 1)
V_MULEO(vmulesb, s16, 8, s8, int16_t, 0)
V_MULEO(vmulosb, s16, 8, s8, int16_t, 1)
V_MULEO(vmuleuh, u32, 4, u16, uint32_t, 0)
V_MULEO(vmulouh, u32, 4, u16, uint32_t, 1)
V_MULEO(vmulesh, s32, 4, s16, int32_t, 0)
V_MULEO(vmulosh, s32, 4, s16, int32_t, 1)

static inline v128 vmsumuhm(v128 a, v128 b, v128 c) {
    v128 r; for (int i = 0; i < 4; i++)
        r.u32[i] = c.u32[i] + (uint32_t)a.u16[2 * i] * b.u16[2 * i] + (uint32_t)a.u16[2 * i + 1] * b.u16[2 * i + 1];
    return r;
}
static inline v128 vmsumuhs(v128 a, v128 b, v128 c) {
    v128 r; for (int i = 0; i < 4; i++)
        r.u32[i] = (uint32_t)sat_s((int64_t)c.u32[i] + (uint64_t)a.u16[2 * i] * b.u16[2 * i] + (uint64_t)a.u16[2 * i + 1] * b.u16[2 * i + 1], 0, 0xffffffffll);
    return r;
}
static inline v128 vmsumshm(v128 a, v128 b, v128 c) {
    v128 r; for (int i = 0; i < 4; i++)
        r.s32[i] = (int32_t)((uint32_t)c.s32[i] + (uint32_t)(a.s16[2 * i] * b.s16[2 * i]) + (uint32_t)(a.s16[2 * i + 1] * b.s16[2 * i + 1]));
    return r;
}
static inline v128 vmsumshs(v128 a, v128 b, v128 c) {
    v128 r; for (int i = 0; i < 4; i++)
        r.s32[i] = (int32_t)sat_s((int64_t)c.s32[i] + a.s16[2 * i] * b.s16[2 * i] + a.s16[2 * i + 1] * b.s16[2 * i + 1], INT32_MIN, INT32_MAX);
    return r;
}
static inline v128 vmsumubm(v128 a, v128 b, v128 c) {
    v128 r; for (int i = 0; i < 4; i++) { uint32_t s = c.u32[i]; for (int j = 0; j < 4; j++) s += (uint32_t)a.u8[4 * i + j] * b.u8[4 * i + j]; r.u32[i] = s; }
    return r;
}
static inline v128 vmsummbm(v128 a, v128 b, v128 c) {
    v128 r; for (int i = 0; i < 4; i++) { int32_t s = c.s32[i]; for (int j = 0; j < 4; j++) s += (int32_t)a.s8[4 * i + j] * b.u8[4 * i + j]; r.s32[i] = s; }
    return r;
}
static inline v128 vmhaddshs(v128 a, v128 b, v128 c) {
    v128 r; for (int i = 0; i < 8; i++) r.s16[i] = (int16_t)sat_s(((a.s16[i] * b.s16[i]) >> 15) + c.s16[i], -32768, 32767); return r;
}
static inline v128 vmhraddshs(v128 a, v128 b, v128 c) {
    v128 r; for (int i = 0; i < 8; i++) r.s16[i] = (int16_t)sat_s(((a.s16[i] * b.s16[i] + 0x4000) >> 15) + c.s16[i], -32768, 32767); return r;
}
static inline v128 vmladduhm(v128 a, v128 b, v128 c) {
    v128 r; for (int i = 0; i < 8; i++) r.u16[i] = (uint16_t)(a.u16[i] * b.u16[i] + c.u16[i]); return r;
}
static inline v128 vsum4ubs(v128 a, v128 b) {
    v128 r; for (int i = 0; i < 4; i++) { int64_t s = b.u32[i]; for (int j = 0; j < 4; j++) s += a.u8[4 * i + j]; r.u32[i] = (uint32_t)sat_s(s, 0, 0xffffffffll); } return r;
}
static inline v128 vsum4sbs(v128 a, v128 b) {
    v128 r; for (int i = 0; i < 4; i++) { int64_t s = b.s32[i]; for (int j = 0; j < 4; j++) s += a.s8[4 * i + j]; r.s32[i] = (int32_t)sat_s(s, INT32_MIN, INT32_MAX); } return r;
}
static inline v128 vsum4shs(v128 a, v128 b) {
    v128 r; for (int i = 0; i < 4; i++) r.s32[i] = (int32_t)sat_s((int64_t)b.s32[i] + a.s16[2 * i] + a.s16[2 * i + 1], INT32_MIN, INT32_MAX); return r;
}
static inline v128 vsum2sws(v128 a, v128 b) {
    v128 r = {{0}};
    r.s32[BEW(1)] = (int32_t)sat_s((int64_t)a.s32[BEW(0)] + a.s32[BEW(1)] + b.s32[BEW(1)], INT32_MIN, INT32_MAX);
    r.s32[BEW(3)] = (int32_t)sat_s((int64_t)a.s32[BEW(2)] + a.s32[BEW(3)] + b.s32[BEW(3)], INT32_MIN, INT32_MAX);
    return r;
}
static inline v128 vsumsws(v128 a, v128 b) {
    v128 r = {{0}};
    r.s32[BEW(3)] = (int32_t)sat_s((int64_t)a.s32[0] + a.s32[1] + a.s32[2] + a.s32[3] + b.s32[BEW(3)], INT32_MIN, INT32_MAX);
    return r;
}

/* ---- float ---- */
V_LANES(vaddfp, f32, 4, a.f32[i] + b.f32[i])
V_LANES(vsubfp, f32, 4, a.f32[i] - b.f32[i])
V_LANES(vmaxfp, f32, 4, a.f32[i] > b.f32[i] ? a.f32[i] : b.f32[i])
V_LANES(vminfp, f32, 4, a.f32[i] < b.f32[i] ? a.f32[i] : b.f32[i])
static inline v128 vmaddfp(v128 a, v128 b, v128 c) { /* a*c+b, operand order as encoded */
    v128 r; for (int i = 0; i < 4; i++) r.f32[i] = fmaf(a.f32[i], c.f32[i], b.f32[i]); return r;
}
static inline v128 vnmsubfp(v128 a, v128 b, v128 c) {
    v128 r; for (int i = 0; i < 4; i++) r.f32[i] = -fmaf(a.f32[i], c.f32[i], -b.f32[i]); return r;
}
#define V_UNF(name, expr) static inline v128 name(v128 b) { v128 r; for (int i = 0; i < 4; i++) { float x = b.f32[i]; r.f32[i] = (expr); } return r; }
V_UNF(vrefp, 1.0f / x)
V_UNF(vrsqrtefp, 1.0f / sqrtf(x))
V_UNF(vexptefp, exp2f(x))
V_UNF(vlogefp, log2f(x))
V_UNF(vrfin, nearbyintf(x))
V_UNF(vrfiz, truncf(x))
V_UNF(vrfip, ceilf(x))
V_UNF(vrfim, floorf(x))
static inline v128 vcfux(v128 b, int s) { v128 r; for (int i = 0; i < 4; i++) r.f32[i] = ldexpf((float)b.u32[i], -s); return r; }
static inline v128 vcfsx(v128 b, int s) { v128 r; for (int i = 0; i < 4; i++) r.f32[i] = ldexpf((float)b.s32[i], -s); return r; }
static inline v128 vctsxs(v128 b, int s) {
    v128 r; for (int i = 0; i < 4; i++) {
        double x = ldexp((double)b.f32[i], s);
        r.s32[i] = x != x ? 0 : x >= 2147483647.0 ? INT32_MAX : x <= -2147483648.0 ? INT32_MIN : (int32_t)x;
    } return r;
}
static inline v128 vctuxs(v128 b, int s) {
    v128 r; for (int i = 0; i < 4; i++) {
        double x = ldexp((double)b.f32[i], s);
        r.u32[i] = x != x ? 0 : x >= 4294967295.0 ? 0xffffffffu : x <= 0 ? 0 : (uint32_t)x;
    } return r;
}

/* ---- compares (all-ones lanes on true) ---- */
V_LANES(vcmpequb, u8, 16, a.u8[i] == b.u8[i] ? 0xff : 0)
V_LANES(vcmpequh, u16, 8, a.u16[i] == b.u16[i] ? 0xffff : 0)
V_LANES(vcmpequw, u32, 4, a.u32[i] == b.u32[i] ? ~0u : 0)
V_LANES(vcmpeqfp, u32, 4, a.f32[i] == b.f32[i] ? ~0u : 0)
V_LANES(vcmpgefp, u32, 4, a.f32[i] >= b.f32[i] ? ~0u : 0)
V_LANES(vcmpgtfp, u32, 4, a.f32[i] > b.f32[i] ? ~0u : 0)
V_LANES(vcmpgtub, u8, 16, a.u8[i] > b.u8[i] ? 0xff : 0)
V_LANES(vcmpgtuh, u16, 8, a.u16[i] > b.u16[i] ? 0xffff : 0)
V_LANES(vcmpgtuw, u32, 4, a.u32[i] > b.u32[i] ? ~0u : 0)
V_LANES(vcmpgtsb, u8, 16, a.s8[i] > b.s8[i] ? 0xff : 0)
V_LANES(vcmpgtsh, u16, 8, a.s16[i] > b.s16[i] ? 0xffff : 0)
V_LANES(vcmpgtsw, u32, 4, a.s32[i] > b.s32[i] ? ~0u : 0)
V_LANES(vcmpbfp, u32, 4, (a.f32[i] <= b.f32[i] ? 0 : 0x80000000u) | (a.f32[i] >= -b.f32[i] ? 0 : 0x40000000u))
/* CR6 after a recording compare: LT = all lanes true, EQ = no lane true */
static inline uint8_t vcr6(v128 r) {
    uint64_t all = r.u64[0] & r.u64[1], any = r.u64[0] | r.u64[1];
    return (all == ~0ull ? 8 : 0) | (any == 0 ? 2 : 0);
}

/* ---- permutes ---- */
static inline v128 vperm(v128 a, v128 b, v128 c) {
    v128 r; for (int i = 0; i < 16; i++) {
        unsigned s = c.u8[BEB(i)] & 31;
        r.u8[BEB(i)] = s < 16 ? a.u8[BEB(s)] : b.u8[BEB(s - 16)];
    } return r;
}
static inline v128 vsldoi(v128 a, v128 b, int sh) {
    v128 r; for (int i = 0; i < 16; i++) { int s = i + sh; r.u8[BEB(i)] = s < 16 ? a.u8[BEB(s)] : b.u8[BEB(s - 16)]; } return r;
}
#define V_MERGE(name, f, n, off) static inline v128 name(v128 a, v128 b) { v128 r; \
    for (int i = 0; i < n / 2; i++) { r.f[(n - 1) - 2 * i] = a.f[(n - 1) - (i + off)]; r.f[(n - 1) - (2 * i + 1)] = b.f[(n - 1) - (i + off)]; } return r; }
V_MERGE(vmrghb, u8, 16, 0)
V_MERGE(vmrglb, u8, 16, 8)
V_MERGE(vmrghh, u16, 8, 0)
V_MERGE(vmrglh, u16, 8, 4)
V_MERGE(vmrghw, u32, 4, 0)
V_MERGE(vmrglw, u32, 4, 2)
static inline v128 vspltb(v128 b, int i) { v128 r; memset(r.u8, b.u8[BEB(i & 15)], 16); return r; }
static inline v128 vsplth(v128 b, int i) { v128 r; for (int k = 0; k < 8; k++) r.u16[k] = b.u16[BEH(i & 7)]; return r; }
static inline v128 vspltw(v128 b, int i) { v128 r; for (int k = 0; k < 4; k++) r.u32[k] = b.u32[BEW(i & 3)]; return r; }
static inline v128 vspltisb(int s) { v128 r; memset(r.u8, (int8_t)s, 16); return r; }
static inline v128 vspltish(int s) { v128 r; for (int k = 0; k < 8; k++) r.s16[k] = (int16_t)s; return r; }
static inline v128 vspltisw(int s) { v128 r; for (int k = 0; k < 4; k++) r.s32[k] = s; return r; }

/* ---- pack / unpack: BE result lane i comes from a (first half) or b ---- */
#define V_PACK(name, df, dn, sf, ty, cvt) static inline v128 name(v128 a, v128 b) { v128 r; \
    for (int i = 0; i < dn; i++) { int k = i % (dn / 2); ty x = i < dn / 2 ? a.sf[(dn / 2 - 1) - k] : b.sf[(dn / 2 - 1) - k]; \
        r.df[(dn - 1) - i] = cvt; } return r; }
V_PACK(vpkuhum, u8, 16, u16, uint16_t, (uint8_t)x)
V_PACK(vpkuwum, u16, 8, u32, uint32_t, (uint16_t)x)
V_PACK(vpkuhus, u8, 16, u16, uint16_t, (uint8_t)(x > 255 ? 255 : x))
V_PACK(vpkuwus, u16, 8, u32, uint32_t, (uint16_t)(x > 65535 ? 65535 : x))
V_PACK(vpkshus, u8, 16, s16, int16_t, (uint8_t)sat_s(x, 0, 255))
V_PACK(vpkswus, u16, 8, s32, int32_t, (uint16_t)sat_s(x, 0, 65535))
V_PACK(vpkshss, s8, 16, s16, int16_t, (int8_t)sat_s(x, -128, 127))
V_PACK(vpkswss, s16, 8, s32, int32_t, (int16_t)sat_s(x, -32768, 32767))
#define V_UNPK(name, df, dn, sf, off) static inline v128 name(v128 b) { v128 r; \
    for (int i = 0; i < dn; i++) r.df[(dn - 1) - i] = b.sf[(2 * dn - 1) - (i + off)]; return r; }
V_UNPK(vupkhsb, s16, 8, s8, 0)
V_UNPK(vupklsb, s16, 8, s8, 8)
V_UNPK(vupkhsh, s32, 4, s16, 0)
V_UNPK(vupklsh, s32, 4, s16, 4)
static inline v128 vpkpx(v128 a, v128 b) {
    v128 r; for (int i = 0; i < 8; i++) {
        uint32_t x = i < 4 ? a.u32[BEW(i)] : b.u32[BEW(i - 4)];
        r.u16[BEH(i)] = (uint16_t)(((x >> 9) & 0x8000) | ((x >> 9) & 0x7c00) | ((x >> 6) & 0x3e0) | ((x >> 3) & 0x1f));
    } return r;
}
static inline v128 vupkpx_(v128 b, int off) {
    v128 r; for (int i = 0; i < 4; i++) {
        uint16_t x = b.u16[BEH(i + off)];
        r.u32[BEW(i)] = ((x & 0x8000) ? 0xff000000u : 0) | ((uint32_t)((x >> 10) & 31) << 16) | ((uint32_t)((x >> 5) & 31) << 8) | (x & 31);
    } return r;
}
#define vupkhpx(b) vupkpx_(b, 0)
#define vupklpx(b) vupkpx_(b, 4)

/* ---- partial loads/stores ---- */
static inline v128 lvsl_(uint64_t ea) { v128 r; unsigned s = EA(ea) & 15; for (int i = 0; i < 16; i++) r.u8[BEB(i)] = (uint8_t)(s + i); return r; }
static inline v128 lvsr_(uint64_t ea) { v128 r; unsigned s = EA(ea) & 15; for (int i = 0; i < 16; i++) r.u8[BEB(i)] = (uint8_t)(16 - s + i); return r; }
static inline v128 lvlx_(uint64_t ea) {
    v128 r = {{0}}; uint32_t a = EA(ea); unsigned s = a & 15;
    for (unsigned i = 0; i < 16 - s; i++) r.u8[BEB(i)] = g_mem[a + i];
    return r;
}
static inline v128 lvrx_(uint64_t ea) {
    v128 r = {{0}}; uint32_t a = EA(ea); unsigned s = a & 15;
    for (unsigned i = 16 - s; i < 16 && s; i++) r.u8[BEB(i)] = g_mem[(a & ~15u) + i - (16 - s)];
    return r;
}
static inline void stvlx_(uint64_t ea, v128 v) {
    uint32_t a = EA(ea); unsigned s = a & 15;
    for (unsigned i = 0; i < 16 - s; i++) g_mem[a + i] = v.u8[BEB(i)];
}
static inline void stvrx_(uint64_t ea, v128 v) {
    uint32_t a = EA(ea); unsigned s = a & 15;
    for (unsigned i = 16 - s; i < 16 && s; i++) g_mem[(a & ~15u) + i - (16 - s)] = v.u8[BEB(i)];
}
static inline void stvebx_(uint64_t ea, v128 v) { uint32_t a = EA(ea); g_mem[a] = v.u8[BEB(a & 15)]; }
static inline void stvehx_(uint64_t ea, v128 v) { uint32_t a = EA(ea) & ~1u; st16(a, v.u16[BEH((a & 15) >> 1)]); }
static inline void stvewx_(uint64_t ea, v128 v) { uint32_t a = EA(ea) & ~3u; st32(a, v.u32[BEW((a & 15) >> 2)]); }
