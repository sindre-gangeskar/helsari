// SPU instruction semantics, shared by the interpreter (spu_interp.cpp) and
// recompiled SPU code (build/generated/spu): step<OP>(thread, instruction, pc)
// executes one instruction and returns the next pc. Generated code calls it
// with constant instructions, so decoding folds away.
#pragma once
#include "spu.h"
#include "runtime.h"

#include <cfloat>
#include <cmath>
#include <cstdlib>

namespace spu {

// The SPU instruction set: X(enum, mnemonic, opcode bits, opcode, operand format).
// fmt: R = rt,ra,rb  I = rt,ra,i7  T = rt,ra,i10  S = rt,i16  L = rt,i18  Q = rt,ra,rb,rc
//      8 = rt,ra,i8  N = none  B = branch i16  C = channel
// tools/recomp/spu.py reads this list.
#define SPU_OPS(X) \
    X(INVALID, "?", 0, 0, 'N') \
    X(STOP, "stop", 11, 0x000, 'N') \
    X(LNOP, "lnop", 11, 0x001, 'N') \
    X(SYNC, "sync", 11, 0x002, 'N') \
    X(DSYNC, "dsync", 11, 0x003, 'N') \
    X(MFSPR, "mfspr", 11, 0x00c, 'R') \
    X(RDCH, "rdch", 11, 0x00d, 'C') \
    X(RCHCNT, "rchcnt", 11, 0x00f, 'C') \
    X(SF, "sf", 11, 0x040, 'R') \
    X(OR, "or", 11, 0x041, 'R') \
    X(BG, "bg", 11, 0x042, 'R') \
    X(SFH, "sfh", 11, 0x048, 'R') \
    X(NOR, "nor", 11, 0x049, 'R') \
    X(ABSDB, "absdb", 11, 0x053, 'R') \
    X(ROT, "rot", 11, 0x058, 'R') \
    X(ROTM, "rotm", 11, 0x059, 'R') \
    X(ROTMA, "rotma", 11, 0x05a, 'R') \
    X(SHL, "shl", 11, 0x05b, 'R') \
    X(ROTH, "roth", 11, 0x05c, 'R') \
    X(ROTHM, "rothm", 11, 0x05d, 'R') \
    X(ROTMAH, "rotmah", 11, 0x05e, 'R') \
    X(SHLH, "shlh", 11, 0x05f, 'R') \
    X(ROTI, "roti", 11, 0x078, 'I') \
    X(ROTMI, "rotmi", 11, 0x079, 'I') \
    X(ROTMAI, "rotmai", 11, 0x07a, 'I') \
    X(SHLI, "shli", 11, 0x07b, 'I') \
    X(ROTHI, "rothi", 11, 0x07c, 'I') \
    X(ROTHMI, "rothmi", 11, 0x07d, 'I') \
    X(ROTMAHI, "rotmahi", 11, 0x07e, 'I') \
    X(SHLHI, "shlhi", 11, 0x07f, 'I') \
    X(A, "a", 11, 0x0c0, 'R') \
    X(AND, "and", 11, 0x0c1, 'R') \
    X(CG, "cg", 11, 0x0c2, 'R') \
    X(AH, "ah", 11, 0x0c8, 'R') \
    X(NAND, "nand", 11, 0x0c9, 'R') \
    X(AVGB, "avgb", 11, 0x0d3, 'R') \
    X(MTSPR, "mtspr", 11, 0x10c, 'R') \
    X(WRCH, "wrch", 11, 0x10d, 'C') \
    X(BIZ, "biz", 11, 0x128, 'R') \
    X(BINZ, "binz", 11, 0x129, 'R') \
    X(BIHZ, "bihz", 11, 0x12a, 'R') \
    X(BIHNZ, "bihnz", 11, 0x12b, 'R') \
    X(STOPD, "stopd", 11, 0x140, 'N') \
    X(STQX, "stqx", 11, 0x144, 'R') \
    X(BI, "bi", 11, 0x1a8, 'R') \
    X(BISL, "bisl", 11, 0x1a9, 'R') \
    X(IRET, "iret", 11, 0x1aa, 'N') \
    X(BISLED, "bisled", 11, 0x1ab, 'R') \
    X(HBR, "hbr", 11, 0x1ac, 'N') \
    X(GB, "gb", 11, 0x1b0, 'R') \
    X(GBH, "gbh", 11, 0x1b1, 'R') \
    X(GBB, "gbb", 11, 0x1b2, 'R') \
    X(FSM, "fsm", 11, 0x1b4, 'R') \
    X(FSMH, "fsmh", 11, 0x1b5, 'R') \
    X(FSMB, "fsmb", 11, 0x1b6, 'R') \
    X(FREST, "frest", 11, 0x1b8, 'R') \
    X(FRSQEST, "frsqest", 11, 0x1b9, 'R') \
    X(LQX, "lqx", 11, 0x1c4, 'R') \
    X(ROTQBYBI, "rotqbybi", 11, 0x1cc, 'R') \
    X(ROTQMBYBI, "rotqmbybi", 11, 0x1cd, 'R') \
    X(SHLQBYBI, "shlqbybi", 11, 0x1cf, 'R') \
    X(CBX, "cbx", 11, 0x1d4, 'R') \
    X(CHX, "chx", 11, 0x1d5, 'R') \
    X(CWX, "cwx", 11, 0x1d6, 'R') \
    X(CDX, "cdx", 11, 0x1d7, 'R') \
    X(ROTQBI, "rotqbi", 11, 0x1d8, 'R') \
    X(ROTQMBI, "rotqmbi", 11, 0x1d9, 'R') \
    X(SHLQBI, "shlqbi", 11, 0x1db, 'R') \
    X(ROTQBY, "rotqby", 11, 0x1dc, 'R') \
    X(ROTQMBY, "rotqmby", 11, 0x1dd, 'R') \
    X(SHLQBY, "shlqby", 11, 0x1df, 'R') \
    X(ORX, "orx", 11, 0x1f0, 'R') \
    X(CBD, "cbd", 11, 0x1f4, 'I') \
    X(CHD, "chd", 11, 0x1f5, 'I') \
    X(CWD, "cwd", 11, 0x1f6, 'I') \
    X(CDD, "cdd", 11, 0x1f7, 'I') \
    X(ROTQBII, "rotqbii", 11, 0x1f8, 'I') \
    X(ROTQMBII, "rotqmbii", 11, 0x1f9, 'I') \
    X(SHLQBII, "shlqbii", 11, 0x1fb, 'I') \
    X(ROTQBYI, "rotqbyi", 11, 0x1fc, 'I') \
    X(ROTQMBYI, "rotqmbyi", 11, 0x1fd, 'I') \
    X(SHLQBYI, "shlqbyi", 11, 0x1ff, 'I') \
    X(NOP, "nop", 11, 0x201, 'N') \
    X(CGT, "cgt", 11, 0x240, 'R') \
    X(XOR, "xor", 11, 0x241, 'R') \
    X(CGTH, "cgth", 11, 0x248, 'R') \
    X(EQV, "eqv", 11, 0x249, 'R') \
    X(CGTB, "cgtb", 11, 0x250, 'R') \
    X(SUMB, "sumb", 11, 0x253, 'R') \
    X(HGT, "hgt", 11, 0x258, 'R') \
    X(CLZ, "clz", 11, 0x2a5, 'R') \
    X(XSWD, "xswd", 11, 0x2a6, 'R') \
    X(XSHW, "xshw", 11, 0x2ae, 'R') \
    X(CNTB, "cntb", 11, 0x2b4, 'R') \
    X(XSBH, "xsbh", 11, 0x2b6, 'R') \
    X(CLGT, "clgt", 11, 0x2c0, 'R') \
    X(ANDC, "andc", 11, 0x2c1, 'R') \
    X(FCGT, "fcgt", 11, 0x2c2, 'R') \
    X(DFCGT, "dfcgt", 11, 0x2c3, 'R') \
    X(FA, "fa", 11, 0x2c4, 'R') \
    X(FS, "fs", 11, 0x2c5, 'R') \
    X(FM, "fm", 11, 0x2c6, 'R') \
    X(CLGTH, "clgth", 11, 0x2c8, 'R') \
    X(ORC, "orc", 11, 0x2c9, 'R') \
    X(FCMGT, "fcmgt", 11, 0x2ca, 'R') \
    X(DFCMGT, "dfcmgt", 11, 0x2cb, 'R') \
    X(DFA, "dfa", 11, 0x2cc, 'R') \
    X(DFS, "dfs", 11, 0x2cd, 'R') \
    X(DFM, "dfm", 11, 0x2ce, 'R') \
    X(CLGTB, "clgtb", 11, 0x2d0, 'R') \
    X(HLGT, "hlgt", 11, 0x2d8, 'R') \
    X(DFMA, "dfma", 11, 0x35c, 'R') \
    X(DFMS, "dfms", 11, 0x35d, 'R') \
    X(DFNMS, "dfnms", 11, 0x35e, 'R') \
    X(DFNMA, "dfnma", 11, 0x35f, 'R') \
    X(CEQ, "ceq", 11, 0x3c0, 'R') \
    X(MPYHHU, "mpyhhu", 11, 0x3ce, 'R') \
    X(ADDX, "addx", 11, 0x340, 'R') \
    X(SFX, "sfx", 11, 0x341, 'R') \
    X(CGX, "cgx", 11, 0x342, 'R') \
    X(BGX, "bgx", 11, 0x343, 'R') \
    X(MPYHHA, "mpyhha", 11, 0x346, 'R') \
    X(MPYHHAU, "mpyhhau", 11, 0x34e, 'R') \
    X(FSCRRD, "fscrrd", 11, 0x398, 'R') \
    X(FESD, "fesd", 11, 0x3b8, 'R') \
    X(FRDS, "frds", 11, 0x3b9, 'R') \
    X(FSCRWR, "fscrwr", 11, 0x3ba, 'R') \
    X(DFTSV, "dftsv", 11, 0x3bf, 'R') \
    X(FCEQ, "fceq", 11, 0x3c2, 'R') \
    X(DFCEQ, "dfceq", 11, 0x3c3, 'R') \
    X(MPY, "mpy", 11, 0x3c4, 'R') \
    X(MPYH, "mpyh", 11, 0x3c5, 'R') \
    X(MPYHH, "mpyhh", 11, 0x3c6, 'R') \
    X(MPYS, "mpys", 11, 0x3c7, 'R') \
    X(CEQH, "ceqh", 11, 0x3c8, 'R') \
    X(FCMEQ, "fcmeq", 11, 0x3ca, 'R') \
    X(DFCMEQ, "dfcmeq", 11, 0x3cb, 'R') \
    X(MPYU, "mpyu", 11, 0x3cc, 'R') \
    X(CEQB, "ceqb", 11, 0x3d0, 'R') \
    X(FI, "fi", 11, 0x3d4, 'R') \
    X(HEQ, "heq", 11, 0x3d8, 'R') \
    X(CFLTS, "cflts", 10, 0x1d8, '8') \
    X(CFLTU, "cfltu", 10, 0x1d9, '8') \
    X(CSFLT, "csflt", 10, 0x1da, '8') \
    X(CUFLT, "cuflt", 10, 0x1db, '8') \
    X(ORI, "ori", 8, 0x04, 'T') \
    X(ORHI, "orhi", 8, 0x05, 'T') \
    X(ORBI, "orbi", 8, 0x06, 'T') \
    X(SFI, "sfi", 8, 0x0c, 'T') \
    X(SFHI, "sfhi", 8, 0x0d, 'T') \
    X(ANDI, "andi", 8, 0x14, 'T') \
    X(ANDHI, "andhi", 8, 0x15, 'T') \
    X(ANDBI, "andbi", 8, 0x16, 'T') \
    X(AI, "ai", 8, 0x1c, 'T') \
    X(AHI, "ahi", 8, 0x1d, 'T') \
    X(STQD, "stqd", 8, 0x24, 'T') \
    X(LQD, "lqd", 8, 0x34, 'T') \
    X(XORI, "xori", 8, 0x44, 'T') \
    X(XORHI, "xorhi", 8, 0x45, 'T') \
    X(XORBI, "xorbi", 8, 0x46, 'T') \
    X(CGTI, "cgti", 8, 0x4c, 'T') \
    X(CGTHI, "cgthi", 8, 0x4d, 'T') \
    X(CGTBI, "cgtbi", 8, 0x4e, 'T') \
    X(HGTI, "hgti", 8, 0x4f, 'T') \
    X(CLGTI, "clgti", 8, 0x5c, 'T') \
    X(CLGTHI, "clgthi", 8, 0x5d, 'T') \
    X(CLGTBI, "clgtbi", 8, 0x5e, 'T') \
    X(HLGTI, "hlgti", 8, 0x5f, 'T') \
    X(MPYI, "mpyi", 8, 0x74, 'T') \
    X(MPYUI, "mpyui", 8, 0x75, 'T') \
    X(CEQI, "ceqi", 8, 0x7c, 'T') \
    X(CEQHI, "ceqhi", 8, 0x7d, 'T') \
    X(CEQBI, "ceqbi", 8, 0x7e, 'T') \
    X(HEQI, "heqi", 8, 0x7f, 'T') \
    X(BRZ, "brz", 9, 0x40, 'B') \
    X(STQA, "stqa", 9, 0x41, 'S') \
    X(BRNZ, "brnz", 9, 0x42, 'B') \
    X(BRHZ, "brhz", 9, 0x44, 'B') \
    X(BRHNZ, "brhnz", 9, 0x46, 'B') \
    X(STQR, "stqr", 9, 0x47, 'B') \
    X(BRA, "bra", 9, 0x60, 'S') \
    X(LQA, "lqa", 9, 0x61, 'S') \
    X(BRASL, "brasl", 9, 0x62, 'S') \
    X(BR, "br", 9, 0x64, 'B') \
    X(FSMBI, "fsmbi", 9, 0x65, 'S') \
    X(BRSL, "brsl", 9, 0x66, 'B') \
    X(LQR, "lqr", 9, 0x67, 'B') \
    X(IL, "il", 9, 0x81, 'S') \
    X(ILHU, "ilhu", 9, 0x82, 'S') \
    X(ILH, "ilh", 9, 0x83, 'S') \
    X(IOHL, "iohl", 9, 0xc1, 'S') \
    X(HBRA, "hbra", 7, 0x08, 'N') \
    X(HBRR, "hbrr", 7, 0x09, 'N') \
    X(ILA, "ila", 7, 0x21, 'L') \
    X(SELB, "selb", 4, 0x8, 'Q') \
    X(SHUFB, "shufb", 4, 0xb, 'Q') \
    X(MPYA, "mpya", 4, 0xc, 'Q') \
    X(FNMS, "fnms", 4, 0xd, 'Q') \
    X(FMA, "fma", 4, 0xe, 'Q') \
    X(FMS, "fms", 4, 0xf, 'Q')

enum Op : uint8_t {
#define SPU_OP_ENUM(e, name, bits, code, fmt) e,
    SPU_OPS(SPU_OP_ENUM)
#undef SPU_OP_ENUM
    OP_COUNT
};

struct OpInfo { const char *name; int bits; uint32_t code; char fmt; };
inline constexpr OpInfo k_ops[] = {
#define SPU_OP_INFO(e, name, bits, code, fmt) {name, bits, code, fmt},
    SPU_OPS(SPU_OP_INFO)
#undef SPU_OP_INFO
};

// Decode table indexed by the top 11 bits of the instruction.
struct Decoder {
    uint8_t t[2048] = {};
    constexpr Decoder() {
        // Narrower opcodes first so exact 11-bit entries win.
        for (int bits : {4, 7, 8, 9, 10, 11})
            for (int op = 1; op < OP_COUNT; op++)
                if (k_ops[op].bits == bits) {
                    int span = 1 << (11 - bits);
                    for (int i = 0; i < span; i++) t[(k_ops[op].code << (11 - bits)) | i] = (uint8_t)op;
                }
    }
};
inline constexpr Decoder g_dec;

inline Op decode(uint32_t x) { return (Op)g_dec.t[x >> 21]; }
inline int32_t sext(uint32_t v, int bits) { return (int32_t)(v << (32 - bits)) >> (32 - bits); }

// Flush single-precision denormals to zero, as the SPU does.
inline float ftz(float f) { return std::fpclassify(f) == FP_SUBNORMAL ? std::copysign(0.0f, f) : f; }
// SPU single precision has no infinities or NaNs: exponent 255 is an ordinary (huge)
// number, and results saturate at the largest magnitude instead of overflowing. Code
// relies on it - e.g. 1/0 followed by huge * 0 = 0 - where IEEE gives inf * 0 = NaN,
// which the final float->int conversion turned into 0 (Killzone 2's per-object
// lighting probes came out black). spf: an operand; spr: a result.
inline float spf(float f) { return std::isfinite(f) ? ftz(f) : std::copysign(FLT_MAX, f); }
inline float spr(float f) { return std::isfinite(f) ? ftz(f) : std::isnan(f) ? 0.0f : std::copysign(FLT_MAX, f); }

inline V splat(uint32_t v) { return V{{v, v, v, v}}; }

// 128-bit helpers on the big-endian register image.
// Big-endian byte i of a register is host byte i ^ 3: one shuffle converts either way.
inline void to_bytes(const V &v, uint8_t *o) {
    _mm_storeu_si128((__m128i *)o, _mm_shuffle_epi8(_mm_loadu_si128((const __m128i *)v.w), SpuThread::bswap32_mask()));
}
inline V from_bytes(const uint8_t *o) {
    V r;
    _mm_storeu_si128((__m128i *)r.w, _mm_shuffle_epi8(_mm_loadu_si128((const __m128i *)o), SpuThread::bswap32_mask()));
    return r;
}
inline V shl_bits(const V &a, int s) {   // s in 0..7
    if (!s) return a;
    V r;
    for (int i = 0; i < 4; i++) r.w[i] = (a.w[i] << s) | (i < 3 ? a.w[i + 1] >> (32 - s) : 0);
    return r;
}
inline V shr_bits(const V &a, int s) {   // s in 0..7
    if (!s) return a;
    V r;
    for (int i = 0; i < 4; i++) r.w[i] = (a.w[i] >> s) | (i > 0 ? a.w[i - 1] << (32 - s) : 0);
    return r;
}
inline V rot_bits(const V &a, int s) {   // s in 0..7
    if (!s) return a;
    V r;
    for (int i = 0; i < 4; i++) r.w[i] = (a.w[i] << s) | (a.w[(i + 1) & 3] >> (32 - s));
    return r;
}
inline V rot_bytes(const V &a, int n) { uint8_t s[16], d[16]; to_bytes(a, s); for (int i = 0; i < 16; i++) d[i] = s[(i + n) & 15]; return from_bytes(d); }
inline V shl_bytes(const V &a, int n) { uint8_t s[16], d[16]; to_bytes(a, s); for (int i = 0; i < 16; i++) d[i] = i + n < 16 ? s[i + n] : 0; return from_bytes(d); }
inline V shr_bytes(const V &a, int n) { uint8_t s[16], d[16]; to_bytes(a, s); for (int i = 0; i < 16; i++) d[i] = i - n >= 0 ? s[i - n] : 0; return from_bytes(d); }

// cbd/chd/cwd/cdd: a shufb control that inserts a preferred-slot element of
// `size` bytes at offset t (byte: 03, half: 02 03, word: 00..03, dword: 00..07)
inline V insert_mask(uint32_t t, int size) {
    V r{{0x10111213u, 0x14151617u, 0x18191a1bu, 0x1c1d1e1fu}};
    t &= 0xf & ~(size - 1);
    static const uint8_t pat1[1] = {3}, pat2[2] = {2, 3}, pat4[4] = {0, 1, 2, 3}, pat8[8] = {0, 1, 2, 3, 4, 5, 6, 7};
    const uint8_t *p = size == 1 ? pat1 : size == 2 ? pat2 : size == 4 ? pat4 : pat8;
    for (int i = 0; i < size; i++) r.set_b(t + i, p[i]);
    return r;
}

// 2^n as a double (n well inside the exponent range): scaling by it is exact,
// like ldexp, without a library call.
inline double pow2(int n) {
    uint64_t b = (uint64_t)(1023 + n) << 52;
    double d;
    memcpy(&d, &b, 8);
    return d;
}

inline int32_t sat_s32(double v) { return v >= 2147483647.0 ? 0x7fffffff : v <= -2147483648.0 ? (int32_t)0x80000000 : std::isnan(v) ? 0 : (int32_t)v; }
inline uint32_t sat_u32(double v) { return v >= 4294967295.0 ? 0xffffffffu : v <= 0 || std::isnan(v) ? 0 : (uint32_t)v; }

#define SPU_INLINE inline __attribute__((always_inline))

// One instruction on register file gpr: t.gpr for the interpreter, a function-local
// copy in recompiled code (tools/recomp/spu.py), which the compiler can keep in
// host registers - t.gpr itself may be aliased by every LS store.
template <Op OP> SPU_INLINE uint32_t step_r(SpuThread &t, V *gpr, uint32_t x, uint32_t pc) {
    uint32_t next = (pc + 4) & (LS_SIZE - 4);
    uint32_t rt = x & 0x7f, ra = (x >> 7) & 0x7f, rb = (x >> 14) & 0x7f;
    const V &va = gpr[ra], &vb = gpr[rb];
    V r{};
    int32_t i7 = sext(rb, 7), i10 = sext((x >> 14) & 0x3ff, 10);
    uint32_t i16 = (x >> 7) & 0xffff;
    int32_t s16 = sext(i16, 16);
    (void)va, (void)vb, (void)r, (void)i7, (void)i10, (void)s16;
    // Indirect branches carry interrupt enable (E, bit 18) / disable (D, bit 19) flags.
    if constexpr (OP == BI || OP == BISL || OP == BIZ || OP == BINZ || OP == BIHZ || OP == BIHNZ || OP == IRET ||
                  OP == BISLED) {
        if (x & (1u << 18)) t.interrupts = true;
        if (x & (1u << 19)) t.interrupts = false;
    }
    switch (OP) {
    /* ---------- memory ---------- */
    case LQD: gpr[rt] = t.lq((va.w[0] + (i10 << 4)) & (LS_SIZE - 16)); break;
    case LQX: gpr[rt] = t.lq((va.w[0] + vb.w[0]) & (LS_SIZE - 16)); break;
    case LQA: gpr[rt] = t.lq((i16 << 2) & (LS_SIZE - 16)); break;
    case LQR: gpr[rt] = t.lq((pc + (s16 << 2)) & (LS_SIZE - 16)); break;
    case STQD: t.stq((va.w[0] + (i10 << 4)) & (LS_SIZE - 16), gpr[rt]); break;
    case STQX: t.stq((va.w[0] + vb.w[0]) & (LS_SIZE - 16), gpr[rt]); break;
    case STQA: t.stq((i16 << 2) & (LS_SIZE - 16), gpr[rt]); break;
    case STQR: t.stq((pc + (s16 << 2)) & (LS_SIZE - 16), gpr[rt]); break;
    case CBD: gpr[rt] = insert_mask(va.w[0] + i7, 1); break;
    case CHD: gpr[rt] = insert_mask(va.w[0] + i7, 2); break;
    case CWD: gpr[rt] = insert_mask(va.w[0] + i7, 4); break;
    case CDD: gpr[rt] = insert_mask(va.w[0] + i7, 8); break;
    case CBX: gpr[rt] = insert_mask(va.w[0] + vb.w[0], 1); break;
    case CHX: gpr[rt] = insert_mask(va.w[0] + vb.w[0], 2); break;
    case CWX: gpr[rt] = insert_mask(va.w[0] + vb.w[0], 4); break;
    case CDX: gpr[rt] = insert_mask(va.w[0] + vb.w[0], 8); break;

    /* ---------- constants ---------- */
    case ILH: for (int i = 0; i < 8; i++) r.set_h(i, (uint16_t)i16); gpr[rt] = r; break;
    case ILHU: gpr[rt] = splat(i16 << 16); break;
    case IL: gpr[rt] = splat((uint32_t)s16); break;
    case IOHL: for (int i = 0; i < 4; i++) gpr[rt].w[i] |= i16; break;
    case ILA: gpr[rt] = splat((x >> 7) & 0x3ffff); break;
    case FSMBI: for (int i = 0; i < 16; i++) r.set_b(i, (i16 >> (15 - i)) & 1 ? 0xff : 0); gpr[rt] = r; break;

    /* ---------- integer arithmetic ---------- */
    case AH: for (int i = 0; i < 8; i++) r.set_h(i, va.h(i) + vb.h(i)); gpr[rt] = r; break;
    case AHI: for (int i = 0; i < 8; i++) r.set_h(i, va.h(i) + (uint16_t)i10); gpr[rt] = r; break;
    case A: for (int i = 0; i < 4; i++) r.w[i] = va.w[i] + vb.w[i]; gpr[rt] = r; break;
    case AI: for (int i = 0; i < 4; i++) r.w[i] = va.w[i] + (uint32_t)i10; gpr[rt] = r; break;
    case SFH: for (int i = 0; i < 8; i++) r.set_h(i, vb.h(i) - va.h(i)); gpr[rt] = r; break;
    case SFHI: for (int i = 0; i < 8; i++) r.set_h(i, (uint16_t)i10 - va.h(i)); gpr[rt] = r; break;
    case SF: for (int i = 0; i < 4; i++) r.w[i] = vb.w[i] - va.w[i]; gpr[rt] = r; break;
    case SFI: for (int i = 0; i < 4; i++) r.w[i] = (uint32_t)i10 - va.w[i]; gpr[rt] = r; break;
    case ADDX: for (int i = 0; i < 4; i++) r.w[i] = va.w[i] + vb.w[i] + (gpr[rt].w[i] & 1); gpr[rt] = r; break;
    case CG: for (int i = 0; i < 4; i++) r.w[i] = (uint32_t)(((uint64_t)va.w[i] + vb.w[i]) >> 32); gpr[rt] = r; break;
    case CGX: for (int i = 0; i < 4; i++) r.w[i] = (uint32_t)(((uint64_t)va.w[i] + vb.w[i] + (gpr[rt].w[i] & 1)) >> 32); gpr[rt] = r; break;
    case SFX: for (int i = 0; i < 4; i++) r.w[i] = vb.w[i] - va.w[i] - (1 - (gpr[rt].w[i] & 1)); gpr[rt] = r; break;
    case BG: for (int i = 0; i < 4; i++) r.w[i] = vb.w[i] >= va.w[i]; gpr[rt] = r; break;
    case BGX:
        for (int i = 0; i < 4; i++)
            r.w[i] = (gpr[rt].w[i] & 1) ? vb.w[i] >= va.w[i] : vb.w[i] > va.w[i];
        gpr[rt] = r;
        break;
    case MPY: for (int i = 0; i < 4; i++) r.w[i] = (uint32_t)((int16_t)va.w[i] * (int16_t)vb.w[i]); gpr[rt] = r; break;
    case MPYU: for (int i = 0; i < 4; i++) r.w[i] = (va.w[i] & 0xffff) * (vb.w[i] & 0xffff); gpr[rt] = r; break;
    case MPYI: for (int i = 0; i < 4; i++) r.w[i] = (uint32_t)((int16_t)va.w[i] * i10); gpr[rt] = r; break;
    case MPYUI: for (int i = 0; i < 4; i++) r.w[i] = (va.w[i] & 0xffff) * ((uint32_t)i10 & 0xffff); gpr[rt] = r; break;
    case MPYA: {
        const V &C = gpr[rt];
        for (int i = 0; i < 4; i++) r.w[i] = (uint32_t)((int16_t)va.w[i] * (int16_t)vb.w[i]) + C.w[i];
        gpr[(x >> 21) & 0x7f] = r;
        break;
    }
    case MPYH: for (int i = 0; i < 4; i++) r.w[i] = ((va.w[i] >> 16) * (vb.w[i] & 0xffff)) << 16; gpr[rt] = r; break;
    case MPYS: for (int i = 0; i < 4; i++) r.w[i] = (uint32_t)(((int16_t)va.w[i] * (int16_t)vb.w[i]) >> 16); gpr[rt] = r; break;
    case MPYHH: for (int i = 0; i < 4; i++) r.w[i] = (uint32_t)((int16_t)(va.w[i] >> 16) * (int16_t)(vb.w[i] >> 16)); gpr[rt] = r; break;
    case MPYHHA: for (int i = 0; i < 4; i++) r.w[i] = gpr[rt].w[i] + (uint32_t)((int16_t)(va.w[i] >> 16) * (int16_t)(vb.w[i] >> 16)); gpr[rt] = r; break;
    case MPYHHU: for (int i = 0; i < 4; i++) r.w[i] = (va.w[i] >> 16) * (vb.w[i] >> 16); gpr[rt] = r; break;
    case MPYHHAU: for (int i = 0; i < 4; i++) r.w[i] = gpr[rt].w[i] + (va.w[i] >> 16) * (vb.w[i] >> 16); gpr[rt] = r; break;
    case CLZ: for (int i = 0; i < 4; i++) r.w[i] = va.w[i] ? __builtin_clz(va.w[i]) : 32; gpr[rt] = r; break;
    case CNTB: for (int i = 0; i < 16; i++) r.set_b(i, (uint8_t)__builtin_popcount(va.b(i))); gpr[rt] = r; break;
    case FSMB: for (int i = 0; i < 16; i++) r.set_b(i, (va.w[0] >> (15 - i)) & 1 ? 0xff : 0); gpr[rt] = r; break;
    case FSMH: for (int i = 0; i < 8; i++) r.set_h(i, (va.w[0] >> (7 - i)) & 1 ? 0xffff : 0); gpr[rt] = r; break;
    case FSM: for (int i = 0; i < 4; i++) r.w[i] = (va.w[0] >> (3 - i)) & 1 ? 0xffffffffu : 0; gpr[rt] = r; break;
    case GBB: { uint32_t v = 0; for (int i = 0; i < 16; i++) v = (v << 1) | (va.b(i) & 1); r.w[0] = v; gpr[rt] = r; break; }
    case GBH: { uint32_t v = 0; for (int i = 0; i < 8; i++) v = (v << 1) | (va.h(i) & 1); r.w[0] = v; gpr[rt] = r; break; }
    case GB: { uint32_t v = 0; for (int i = 0; i < 4; i++) v = (v << 1) | (va.w[i] & 1); r.w[0] = v; gpr[rt] = r; break; }
    case AVGB: for (int i = 0; i < 16; i++) r.set_b(i, (uint8_t)((va.b(i) + vb.b(i) + 1) >> 1)); gpr[rt] = r; break;
    case ABSDB: for (int i = 0; i < 16; i++) r.set_b(i, (uint8_t)std::abs(va.b(i) - vb.b(i))); gpr[rt] = r; break;
    case SUMB:
        for (int i = 0; i < 4; i++) {
            uint16_t sb = 0, sa = 0;
            for (int k = 0; k < 4; k++) sb += vb.b(4 * i + k), sa += va.b(4 * i + k);
            r.w[i] = ((uint32_t)sb << 16) | sa;
        }
        gpr[rt] = r;
        break;
    case XSBH: for (int i = 0; i < 8; i++) r.set_h(i, (uint16_t)(int8_t)va.h(i)); gpr[rt] = r; break;
    case XSHW: for (int i = 0; i < 4; i++) r.w[i] = (uint32_t)(int16_t)va.w[i]; gpr[rt] = r; break;
    case XSWD: for (int i = 0; i < 2; i++) r.set_d(i, (uint64_t)(int64_t)(int32_t)va.w[2 * i + 1]); gpr[rt] = r; break;

    /* ---------- logical ---------- */
    case AND: for (int i = 0; i < 4; i++) r.w[i] = va.w[i] & vb.w[i]; gpr[rt] = r; break;
    case ANDC: for (int i = 0; i < 4; i++) r.w[i] = va.w[i] & ~vb.w[i]; gpr[rt] = r; break;
    case ANDBI: for (int i = 0; i < 16; i++) r.set_b(i, va.b(i) & (uint8_t)i10); gpr[rt] = r; break;
    case ANDHI: for (int i = 0; i < 8; i++) r.set_h(i, va.h(i) & (uint16_t)i10); gpr[rt] = r; break;
    case ANDI: for (int i = 0; i < 4; i++) r.w[i] = va.w[i] & (uint32_t)i10; gpr[rt] = r; break;
    case OR: for (int i = 0; i < 4; i++) r.w[i] = va.w[i] | vb.w[i]; gpr[rt] = r; break;
    case ORC: for (int i = 0; i < 4; i++) r.w[i] = va.w[i] | ~vb.w[i]; gpr[rt] = r; break;
    case ORBI: for (int i = 0; i < 16; i++) r.set_b(i, va.b(i) | (uint8_t)i10); gpr[rt] = r; break;
    case ORHI: for (int i = 0; i < 8; i++) r.set_h(i, va.h(i) | (uint16_t)i10); gpr[rt] = r; break;
    case ORI: for (int i = 0; i < 4; i++) r.w[i] = va.w[i] | (uint32_t)i10; gpr[rt] = r; break;
    case ORX: r.w[0] = va.w[0] | va.w[1] | va.w[2] | va.w[3]; gpr[rt] = r; break;
    case XOR: for (int i = 0; i < 4; i++) r.w[i] = va.w[i] ^ vb.w[i]; gpr[rt] = r; break;
    case XORBI: for (int i = 0; i < 16; i++) r.set_b(i, va.b(i) ^ (uint8_t)i10); gpr[rt] = r; break;
    case XORHI: for (int i = 0; i < 8; i++) r.set_h(i, va.h(i) ^ (uint16_t)i10); gpr[rt] = r; break;
    case XORI: for (int i = 0; i < 4; i++) r.w[i] = va.w[i] ^ (uint32_t)i10; gpr[rt] = r; break;
    case NAND: for (int i = 0; i < 4; i++) r.w[i] = ~(va.w[i] & vb.w[i]); gpr[rt] = r; break;
    case NOR: for (int i = 0; i < 4; i++) r.w[i] = ~(va.w[i] | vb.w[i]); gpr[rt] = r; break;
    case EQV: for (int i = 0; i < 4; i++) r.w[i] = ~(va.w[i] ^ vb.w[i]); gpr[rt] = r; break;
    case SELB: {
        const V &C = gpr[rt];
        for (int i = 0; i < 4; i++) r.w[i] = (C.w[i] & vb.w[i]) | (~C.w[i] & va.w[i]);
        gpr[(x >> 21) & 0x7f] = r;
        break;
    }
    case SHUFB: {
        uint8_t ab[32], c[16], o[16];
        to_bytes(va, ab), to_bytes(vb, ab + 16), to_bytes(gpr[rt], c);
        for (int i = 0; i < 16; i++)
            o[i] = c[i] >= 0xe0 ? 0x80 : c[i] >= 0xc0 ? 0xff : c[i] >= 0x80 ? 0x00 : ab[c[i] & 0x1f];
        gpr[(x >> 21) & 0x7f] = from_bytes(o);
        break;
    }

    /* ---------- shifts and rotates ---------- */
    case SHLH: for (int i = 0; i < 8; i++) { int s = vb.h(i) & 0x1f; r.set_h(i, s > 15 ? 0 : (uint16_t)(va.h(i) << s)); } gpr[rt] = r; break;
    case SHLHI: { int s = i7 & 0x1f; for (int i = 0; i < 8; i++) r.set_h(i, s > 15 ? 0 : (uint16_t)(va.h(i) << s)); gpr[rt] = r; break; }
    case SHL: for (int i = 0; i < 4; i++) { int s = vb.w[i] & 0x3f; r.w[i] = s > 31 ? 0 : va.w[i] << s; } gpr[rt] = r; break;
    case SHLI: { int s = i7 & 0x3f; for (int i = 0; i < 4; i++) r.w[i] = s > 31 ? 0 : va.w[i] << s; gpr[rt] = r; break; }
    case ROTH: for (int i = 0; i < 8; i++) { int s = vb.h(i) & 0xf; uint16_t v = va.h(i); r.set_h(i, (uint16_t)((v << s) | (v >> ((16 - s) & 15)))); } gpr[rt] = r; break;
    case ROTHI: { int s = i7 & 0xf; for (int i = 0; i < 8; i++) { uint16_t v = va.h(i); r.set_h(i, (uint16_t)((v << s) | (v >> ((16 - s) & 15)))); } gpr[rt] = r; break; }
    case ROT: for (int i = 0; i < 4; i++) { int s = vb.w[i] & 0x1f; r.w[i] = (va.w[i] << s) | (s ? va.w[i] >> (32 - s) : 0); } gpr[rt] = r; break;
    case ROTI: { int s = i7 & 0x1f; for (int i = 0; i < 4; i++) r.w[i] = (va.w[i] << s) | (s ? va.w[i] >> (32 - s) : 0); gpr[rt] = r; break; }
    case ROTHM: for (int i = 0; i < 8; i++) { int s = (0 - vb.h(i)) & 0x1f; r.set_h(i, s > 15 ? 0 : (uint16_t)(va.h(i) >> s)); } gpr[rt] = r; break;
    case ROTHMI: { int s = (0 - i7) & 0x1f; for (int i = 0; i < 8; i++) r.set_h(i, s > 15 ? 0 : (uint16_t)(va.h(i) >> s)); gpr[rt] = r; break; }
    case ROTM: for (int i = 0; i < 4; i++) { int s = (0 - vb.w[i]) & 0x3f; r.w[i] = s > 31 ? 0 : va.w[i] >> s; } gpr[rt] = r; break;
    case ROTMI: { int s = (0 - i7) & 0x3f; for (int i = 0; i < 4; i++) r.w[i] = s > 31 ? 0 : va.w[i] >> s; gpr[rt] = r; break; }
    case ROTMAH: for (int i = 0; i < 8; i++) { int s = (0 - vb.h(i)) & 0x1f; r.set_h(i, (uint16_t)((int16_t)va.h(i) >> (s > 15 ? 15 : s))); } gpr[rt] = r; break;
    case ROTMAHI: { int s = (0 - i7) & 0x1f; for (int i = 0; i < 8; i++) r.set_h(i, (uint16_t)((int16_t)va.h(i) >> (s > 15 ? 15 : s))); gpr[rt] = r; break; }
    case ROTMA: for (int i = 0; i < 4; i++) { int s = (0 - vb.w[i]) & 0x3f; r.w[i] = (uint32_t)((int32_t)va.w[i] >> (s > 31 ? 31 : s)); } gpr[rt] = r; break;
    case ROTMAI: { int s = (0 - i7) & 0x3f; for (int i = 0; i < 4; i++) r.w[i] = (uint32_t)((int32_t)va.w[i] >> (s > 31 ? 31 : s)); gpr[rt] = r; break; }
    case SHLQBI: gpr[rt] = shl_bits(va, vb.w[0] & 7); break;
    case SHLQBII: gpr[rt] = shl_bits(va, i7 & 7); break;
    case SHLQBY: gpr[rt] = shl_bytes(va, vb.w[0] & 0x1f); break;
    case SHLQBYI: gpr[rt] = shl_bytes(va, i7 & 0x1f); break;
    case SHLQBYBI: gpr[rt] = shl_bytes(va, (vb.w[0] >> 3) & 0x1f); break;
    case ROTQBI: gpr[rt] = rot_bits(va, vb.w[0] & 7); break;
    case ROTQBII: gpr[rt] = rot_bits(va, i7 & 7); break;
    case ROTQBY: gpr[rt] = rot_bytes(va, vb.w[0] & 0xf); break;
    case ROTQBYI: gpr[rt] = rot_bytes(va, i7 & 0xf); break;
    case ROTQBYBI: gpr[rt] = rot_bytes(va, (vb.w[0] >> 3) & 0xf); break;
    case ROTQMBI: gpr[rt] = shr_bits(va, (0 - vb.w[0]) & 7); break;
    case ROTQMBII: gpr[rt] = shr_bits(va, (0 - i7) & 7); break;
    case ROTQMBY: gpr[rt] = shr_bytes(va, (0 - vb.w[0]) & 0x1f); break;
    case ROTQMBYI: gpr[rt] = shr_bytes(va, (0 - i7) & 0x1f); break;
    case ROTQMBYBI: gpr[rt] = shr_bytes(va, (0 - (vb.w[0] >> 3)) & 0x1f); break;

    /* ---------- compares ---------- */
    case CEQB: for (int i = 0; i < 16; i++) r.set_b(i, va.b(i) == vb.b(i) ? 0xff : 0); gpr[rt] = r; break;
    case CEQBI: for (int i = 0; i < 16; i++) r.set_b(i, va.b(i) == (uint8_t)i10 ? 0xff : 0); gpr[rt] = r; break;
    case CEQH: for (int i = 0; i < 8; i++) r.set_h(i, va.h(i) == vb.h(i) ? 0xffff : 0); gpr[rt] = r; break;
    case CEQHI: for (int i = 0; i < 8; i++) r.set_h(i, va.h(i) == (uint16_t)i10 ? 0xffff : 0); gpr[rt] = r; break;
    case CEQ: for (int i = 0; i < 4; i++) r.w[i] = va.w[i] == vb.w[i] ? ~0u : 0; gpr[rt] = r; break;
    case CEQI: for (int i = 0; i < 4; i++) r.w[i] = va.w[i] == (uint32_t)i10 ? ~0u : 0; gpr[rt] = r; break;
    case CGTB: for (int i = 0; i < 16; i++) r.set_b(i, (int8_t)va.b(i) > (int8_t)vb.b(i) ? 0xff : 0); gpr[rt] = r; break;
    case CGTBI: for (int i = 0; i < 16; i++) r.set_b(i, (int8_t)va.b(i) > (int8_t)(uint8_t)i10 ? 0xff : 0); gpr[rt] = r; break;
    case CGTH: for (int i = 0; i < 8; i++) r.set_h(i, (int16_t)va.h(i) > (int16_t)vb.h(i) ? 0xffff : 0); gpr[rt] = r; break;
    case CGTHI: for (int i = 0; i < 8; i++) r.set_h(i, (int16_t)va.h(i) > (int16_t)i10 ? 0xffff : 0); gpr[rt] = r; break;
    case CGT: for (int i = 0; i < 4; i++) r.w[i] = (int32_t)va.w[i] > (int32_t)vb.w[i] ? ~0u : 0; gpr[rt] = r; break;
    case CGTI: for (int i = 0; i < 4; i++) r.w[i] = (int32_t)va.w[i] > i10 ? ~0u : 0; gpr[rt] = r; break;
    case CLGTB: for (int i = 0; i < 16; i++) r.set_b(i, va.b(i) > vb.b(i) ? 0xff : 0); gpr[rt] = r; break;
    case CLGTBI: for (int i = 0; i < 16; i++) r.set_b(i, va.b(i) > (uint8_t)i10 ? 0xff : 0); gpr[rt] = r; break;
    case CLGTH: for (int i = 0; i < 8; i++) r.set_h(i, va.h(i) > vb.h(i) ? 0xffff : 0); gpr[rt] = r; break;
    case CLGTHI: for (int i = 0; i < 8; i++) r.set_h(i, va.h(i) > (uint16_t)i10 ? 0xffff : 0); gpr[rt] = r; break;
    case CLGT: for (int i = 0; i < 4; i++) r.w[i] = va.w[i] > vb.w[i] ? ~0u : 0; gpr[rt] = r; break;
    case CLGTI: for (int i = 0; i < 4; i++) r.w[i] = va.w[i] > (uint32_t)i10 ? ~0u : 0; gpr[rt] = r; break;

    /* ---------- halts (raise an error) ---------- */
    case HEQ: case HEQI: case HGT: case HGTI: case HLGT: case HLGTI: {
        uint32_t b = (OP == HEQ || OP == HGT || OP == HLGT) ? vb.w[0] : (uint32_t)i10;
        bool halt = OP == HEQ || OP == HEQI ? va.w[0] == b
                  : OP == HGT || OP == HGTI ? (int32_t)va.w[0] > (int32_t)b
                  : va.w[0] > b;
        if (halt) {
            LOG("spu", "%s: halt instruction at 0x%x", t.name.c_str(), pc);
            t.dump_history();
            t.running = false;
        }
        break;
    }

    /* ---------- branches ---------- */
    case BR: next = (pc + (s16 << 2)) & (LS_SIZE - 4); break;
    case BRA: next = (s16 << 2) & (LS_SIZE - 4); break;
    case BRSL: gpr[rt] = V{{next, 0, 0, 0}}; next = (pc + (s16 << 2)) & (LS_SIZE - 4); break;
    case BRASL: gpr[rt] = V{{next, 0, 0, 0}}; next = (s16 << 2) & (LS_SIZE - 4); break;
    case BI: next = va.w[0] & (LS_SIZE - 4); break;
    case IRET: next = t.srr0 & (LS_SIZE - 4); break;
    case BISLED: case BISL: { uint32_t target = va.w[0] & (LS_SIZE - 4); gpr[rt] = V{{next, 0, 0, 0}}; if (OP == BISL) next = target; break; }
    case BRNZ: if (gpr[rt].w[0] != 0) next = (pc + (s16 << 2)) & (LS_SIZE - 4); break;
    case BRZ: if (gpr[rt].w[0] == 0) next = (pc + (s16 << 2)) & (LS_SIZE - 4); break;
    case BRHNZ: if ((gpr[rt].w[0] & 0xffff) != 0) next = (pc + (s16 << 2)) & (LS_SIZE - 4); break;
    case BRHZ: if ((gpr[rt].w[0] & 0xffff) == 0) next = (pc + (s16 << 2)) & (LS_SIZE - 4); break;
    case BIZ: if (gpr[rt].w[0] == 0) next = va.w[0] & (LS_SIZE - 4); break;
    case BINZ: if (gpr[rt].w[0] != 0) next = va.w[0] & (LS_SIZE - 4); break;
    case BIHZ: if ((gpr[rt].w[0] & 0xffff) == 0) next = va.w[0] & (LS_SIZE - 4); break;
    case BIHNZ: if ((gpr[rt].w[0] & 0xffff) != 0) next = va.w[0] & (LS_SIZE - 4); break;
    case HBR: case HBRA: case HBRR: case NOP: case LNOP: case SYNC: case DSYNC: break;

    /* ---------- single precision ---------- */
    case FA: for (int i = 0; i < 4; i++) r.set_f(i, spr(spf(va.f(i)) + spf(vb.f(i)))); gpr[rt] = r; break;
    case FS: for (int i = 0; i < 4; i++) r.set_f(i, spr(spf(va.f(i)) - spf(vb.f(i)))); gpr[rt] = r; break;
    case FM: for (int i = 0; i < 4; i++) r.set_f(i, spr(spf(va.f(i)) * spf(vb.f(i)))); gpr[rt] = r; break;
    case FMA: { const V &C = gpr[rt]; for (int i = 0; i < 4; i++) r.set_f(i, spr(spr(spf(va.f(i)) * spf(vb.f(i))) + spf(C.f(i)))); gpr[(x >> 21) & 0x7f] = r; break; }
    case FMS: { const V &C = gpr[rt]; for (int i = 0; i < 4; i++) r.set_f(i, spr(spr(spf(va.f(i)) * spf(vb.f(i))) - spf(C.f(i)))); gpr[(x >> 21) & 0x7f] = r; break; }
    case FNMS: { const V &C = gpr[rt]; for (int i = 0; i < 4; i++) r.set_f(i, spr(spf(C.f(i)) - spr(spf(va.f(i)) * spf(vb.f(i))))); gpr[(x >> 21) & 0x7f] = r; break; }
    // frest/frsqest return the exact value; fi then passes it through.
    case FREST: for (int i = 0; i < 4; i++) r.set_f(i, spr(1.0f / spf(va.f(i)))); gpr[rt] = r; break;
    case FRSQEST: for (int i = 0; i < 4; i++) r.set_f(i, spr(1.0f / std::sqrt(std::fabs(spf(va.f(i)))))); gpr[rt] = r; break;
    case FI: gpr[rt] = vb; break;
    case FCEQ: for (int i = 0; i < 4; i++) r.w[i] = ftz(va.f(i)) == ftz(vb.f(i)) ? ~0u : 0; gpr[rt] = r; break;
    case FCMEQ: for (int i = 0; i < 4; i++) r.w[i] = std::fabs(ftz(va.f(i))) == std::fabs(ftz(vb.f(i))) ? ~0u : 0; gpr[rt] = r; break;
    case FCGT: for (int i = 0; i < 4; i++) r.w[i] = ftz(va.f(i)) > ftz(vb.f(i)) ? ~0u : 0; gpr[rt] = r; break;
    case FCMGT: for (int i = 0; i < 4; i++) r.w[i] = std::fabs(ftz(va.f(i))) > std::fabs(ftz(vb.f(i))) ? ~0u : 0; gpr[rt] = r; break;
    case CSFLT: { int sc = 155 - (int)((x >> 14) & 0xff); for (int i = 0; i < 4; i++) r.set_f(i, (float)((double)(int32_t)va.w[i] * pow2(-sc))); gpr[rt] = r; break; }
    case CUFLT: { int sc = 155 - (int)((x >> 14) & 0xff); for (int i = 0; i < 4; i++) r.set_f(i, (float)((double)va.w[i] * pow2(-sc))); gpr[rt] = r; break; }
    case CFLTS: { int sc = 173 - (int)((x >> 14) & 0xff); for (int i = 0; i < 4; i++) r.w[i] = (uint32_t)sat_s32((double)va.f(i) * pow2(sc)); gpr[rt] = r; break; }
    case CFLTU: { int sc = 173 - (int)((x >> 14) & 0xff); for (int i = 0; i < 4; i++) r.w[i] = sat_u32((double)va.f(i) * pow2(sc)); gpr[rt] = r; break; }
    case FSCRRD: gpr[rt] = V{}; break;
    case FSCRWR: break;

    /* ---------- double precision ---------- */
    case FESD: for (int i = 0; i < 2; i++) r.set_df(i, (double)va.f(2 * i)); gpr[rt] = r; break;
    case FRDS: for (int i = 0; i < 2; i++) r.set_f(2 * i, (float)va.df(i)); gpr[rt] = r; break;
    case DFA: for (int i = 0; i < 2; i++) r.set_df(i, va.df(i) + vb.df(i)); gpr[rt] = r; break;
    case DFS: for (int i = 0; i < 2; i++) r.set_df(i, va.df(i) - vb.df(i)); gpr[rt] = r; break;
    case DFM: for (int i = 0; i < 2; i++) r.set_df(i, va.df(i) * vb.df(i)); gpr[rt] = r; break;
    case DFMA: for (int i = 0; i < 2; i++) r.set_df(i, va.df(i) * vb.df(i) + gpr[rt].df(i)); gpr[rt] = r; break;
    case DFMS: for (int i = 0; i < 2; i++) r.set_df(i, va.df(i) * vb.df(i) - gpr[rt].df(i)); gpr[rt] = r; break;
    case DFNMS: for (int i = 0; i < 2; i++) r.set_df(i, gpr[rt].df(i) - va.df(i) * vb.df(i)); gpr[rt] = r; break;
    case DFNMA: for (int i = 0; i < 2; i++) r.set_df(i, -(va.df(i) * vb.df(i) + gpr[rt].df(i))); gpr[rt] = r; break;
    case DFCEQ: for (int i = 0; i < 2; i++) r.set_d(i, va.df(i) == vb.df(i) ? ~0ull : 0); gpr[rt] = r; break;
    case DFCMEQ: for (int i = 0; i < 2; i++) r.set_d(i, std::fabs(va.df(i)) == std::fabs(vb.df(i)) ? ~0ull : 0); gpr[rt] = r; break;
    case DFCGT: for (int i = 0; i < 2; i++) r.set_d(i, va.df(i) > vb.df(i) ? ~0ull : 0); gpr[rt] = r; break;
    case DFCMGT: for (int i = 0; i < 2; i++) r.set_d(i, std::fabs(va.df(i)) > std::fabs(vb.df(i)) ? ~0ull : 0); gpr[rt] = r; break;
    case DFTSV: gpr[rt] = V{}; break;

    /* ---------- channels and control ---------- */
    case RDCH: gpr[rt] = V{{t.read_channel(ra), 0, 0, 0}}; break;
    case RCHCNT: gpr[rt] = V{{t.channel_count(ra), 0, 0, 0}}; break;
    case WRCH: t.write_channel(ra, gpr[rt].w[0]); break;
    case MFSPR: gpr[rt] = V{}; break;
    case MTSPR: break;
    case STOP: case STOPD:
        t.pc = next;
        if (!t.on_stop || !t.on_stop(t, x & 0x3fff)) t.running = false;
        return t.pc;

    case INVALID: default:
        LOG("spu", "%s: unknown instruction %08x at 0x%x", t.name.c_str(), x, pc);
        t.dump_history();
        t.running = false;
        return pc;
    }
    return next;
}

template <Op OP> SPU_INLINE uint32_t step(SpuThread &t, uint32_t x, uint32_t pc) { return step_r<OP>(t, t.gpr, x, pc); }

} // namespace spu
