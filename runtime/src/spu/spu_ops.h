// SPU instruction semantics, shared by the interpreter (spu_interp.cpp) and
// recompiled SPU code (build/generated/spu): step<OP>(thread, instruction, pc)
// executes one instruction and returns the next pc. Generated code calls it
// with constant instructions, so decoding folds away.
#pragma once
#include "spu.h"
#include "runtime.h"

#include <cfloat>
#include <immintrin.h>
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

// The same rules on four lanes at once, branch-free (SSE4.1): bit-identical to spf/spr
// applied per element - the arithmetic between them is the same IEEE single precision.
inline __m128 vf(const V &v) { return _mm_loadu_ps((const float *)v.w); }
inline V fv(__m128 x) { V r; _mm_storeu_ps((float *)r.w, x); return r; }
inline __m128 v_ftz(__m128 x) {
    __m128i b = _mm_castps_si128(x), e = _mm_and_si128(b, _mm_set1_epi32(0x7f800000));
    __m128i den = _mm_cmpeq_epi32(e, _mm_setzero_si128());
    return _mm_castsi128_ps(_mm_blendv_epi8(b, _mm_and_si128(b, _mm_set1_epi32((int)0x80000000)), den));
}
// SPU threads run with DAZ and FTZ set (SpuThread::run): the hardware then treats denormals
// as SPUs do, so only infinities and NaNs (exponent all ones, rare) need the slow paths.
// Checked bit-identical against the full versions on 480M results; twice as fast.
inline bool v_any_big(__m128 x) {
    __m128i e = _mm_and_si128(_mm_castps_si128(x), _mm_set1_epi32(0x7f800000));
    return _mm_movemask_epi8(_mm_cmpeq_epi32(e, _mm_set1_epi32(0x7f800000))) != 0;
}
inline __m128 v_spf_slow(__m128 x) {
    __m128i b = _mm_castps_si128(x), e = _mm_and_si128(b, _mm_set1_epi32(0x7f800000));
    __m128i sign = _mm_and_si128(b, _mm_set1_epi32((int)0x80000000));
    __m128i den = _mm_cmpeq_epi32(e, _mm_setzero_si128()), big = _mm_cmpeq_epi32(e, _mm_set1_epi32(0x7f800000));
    b = _mm_blendv_epi8(b, sign, den);
    b = _mm_blendv_epi8(b, _mm_or_si128(sign, _mm_set1_epi32(0x7f7fffff)), big);
    return _mm_castsi128_ps(b);
}
inline __m128 v_spf(__m128 x) { return v_any_big(x) ? v_spf_slow(x) : x; }
inline __m128 v_spr_slow(__m128 x) {
    __m128i b = _mm_castps_si128(x), e = _mm_and_si128(b, _mm_set1_epi32(0x7f800000));
    __m128i sign = _mm_and_si128(b, _mm_set1_epi32((int)0x80000000));
    __m128i den = _mm_cmpeq_epi32(e, _mm_setzero_si128()), big = _mm_cmpeq_epi32(e, _mm_set1_epi32(0x7f800000));
    __m128i nan = _mm_andnot_si128(_mm_cmpeq_epi32(_mm_and_si128(b, _mm_set1_epi32(0x007fffff)), _mm_setzero_si128()), big);
    b = _mm_blendv_epi8(b, sign, den);
    b = _mm_blendv_epi8(b, _mm_or_si128(sign, _mm_set1_epi32(0x7f7fffff)), big);
    b = _mm_andnot_si128(nan, b);   // NaN -> +0
    return _mm_castsi128_ps(b);
}
inline __m128 v_spr(__m128 x) { return v_any_big(x) ? v_spr_slow(x) : x; }

// Whole-register integer helpers (V keeps each word in host order).
inline __m128i iv(const V &v) { return _mm_loadu_si128((const __m128i *)v.w); }
inline V vi(__m128i x) { V r; _mm_storeu_si128((__m128i *)r.w, x); return r; }
inline __m128i not_(__m128i x) { return _mm_xor_si128(x, _mm_set1_epi32(-1)); }
inline __m128i lo16(__m128i x) { return _mm_and_si128(x, _mm_set1_epi32(0xffff)); }
inline __m128i cmpgt_u32(__m128i a, __m128i b) {
    const __m128i s = _mm_set1_epi32((int)0x80000000);
    return _mm_cmpgt_epi32(_mm_xor_si128(a, s), _mm_xor_si128(b, s));
}
// Float operations with one combined check: is any lane of any operand or result inf or NaN
// (exponent all ones)? If so, the same operation in full, out of line, so the hot code stays small.
inline __m128 f_abs(__m128 x) { return _mm_and_ps(x, _mm_castsi128_ps(_mm_set1_epi32(0x7fffffff))); }
inline bool f_big(__m128i maxabs) { return !_mm_testz_si128(_mm_cmpgt_epi32(maxabs, _mm_set1_epi32(0x7f7fffff)), _mm_set1_epi32(-1)); }
inline __m128i f_max(__m128 a, __m128 b) { return _mm_max_epu32(_mm_castps_si128(f_abs(a)), _mm_castps_si128(f_abs(b))); }
inline __m128i f_max(__m128i m, __m128 b) { return _mm_max_epu32(m, _mm_castps_si128(f_abs(b))); }
#define SPU_COLD __attribute__((noinline, cold))
SPU_COLD inline __m128 f_add_slow(__m128 a, __m128 b) { return v_spr_slow(_mm_add_ps(v_spf_slow(a), v_spf_slow(b))); }
SPU_COLD inline __m128 f_sub_slow(__m128 a, __m128 b) { return v_spr_slow(_mm_sub_ps(v_spf_slow(a), v_spf_slow(b))); }
SPU_COLD inline __m128 f_mul_slow(__m128 a, __m128 b) { return v_spr_slow(_mm_mul_ps(v_spf_slow(a), v_spf_slow(b))); }
SPU_COLD inline __m128 f_madd_slow(__m128 a, __m128 b, __m128 c) { return v_spr_slow(_mm_add_ps(f_mul_slow(a, b), v_spf_slow(c))); }
SPU_COLD inline __m128 f_msub_slow(__m128 a, __m128 b, __m128 c) { return v_spr_slow(_mm_sub_ps(f_mul_slow(a, b), v_spf_slow(c))); }
SPU_COLD inline __m128 f_nmsub_slow(__m128 a, __m128 b, __m128 c) { return v_spr_slow(_mm_sub_ps(v_spf_slow(c), f_mul_slow(a, b))); }
__attribute__((always_inline)) inline __m128 f_add(__m128 a, __m128 b) {
    __m128 r = _mm_add_ps(a, b);
    return __builtin_expect(f_big(f_max(f_max(a, b), r)), 0) ? f_add_slow(a, b) : r;
}
__attribute__((always_inline)) inline __m128 f_sub(__m128 a, __m128 b) {
    __m128 r = _mm_sub_ps(a, b);
    return __builtin_expect(f_big(f_max(f_max(a, b), r)), 0) ? f_sub_slow(a, b) : r;
}
__attribute__((always_inline)) inline __m128 f_mul(__m128 a, __m128 b) {
    __m128 r = _mm_mul_ps(a, b);
    return __builtin_expect(f_big(f_max(f_max(a, b), r)), 0) ? f_mul_slow(a, b) : r;
}
__attribute__((always_inline)) inline __m128 f_madd(__m128 a, __m128 b, __m128 c) {
    __m128 m = _mm_mul_ps(a, b), r = _mm_add_ps(m, c);
    return __builtin_expect(f_big(f_max(f_max(f_max(f_max(a, b), m), c), r)), 0) ? f_madd_slow(a, b, c) : r;
}
__attribute__((always_inline)) inline __m128 f_msub(__m128 a, __m128 b, __m128 c) {
    __m128 m = _mm_mul_ps(a, b), r = _mm_sub_ps(m, c);
    return __builtin_expect(f_big(f_max(f_max(f_max(f_max(a, b), m), c), r)), 0) ? f_msub_slow(a, b, c) : r;
}
__attribute__((always_inline)) inline __m128 f_nmsub(__m128 a, __m128 b, __m128 c) {
    __m128 m = _mm_mul_ps(a, b), r = _mm_sub_ps(c, m);
    return __builtin_expect(f_big(f_max(f_max(f_max(f_max(a, b), m), c), r)), 0) ? f_nmsub_slow(a, b, c) : r;
}

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
// Big-endian byte images of registers in SSE registers (byte i = register byte i),
// for whole-register byte permutes with pshufb.
inline __m128i be_bytes(const V &v) {
    return _mm_shuffle_epi8(_mm_loadu_si128((const __m128i *)v.w), _mm_setr_epi8(3, 2, 1, 0, 7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12));
}
inline V from_be(__m128i b) {
    V r;
    _mm_storeu_si128((__m128i *)r.w, _mm_shuffle_epi8(b, _mm_setr_epi8(3, 2, 1, 0, 7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12)));
    return r;
}
inline __m128i iota_h() { return _mm_setr_epi8(3, 2, 1, 0, 7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12); }
inline __m128i iota8() { return _mm_setr_epi8(0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15); }
// d[i] = s[(i + n) & 15]
inline V rot_bytes(const V &a, int n) {
    // Big-endian byte i is host byte i ^ 3: iota_h is each host byte's big-endian index, and
    // ^ 3 turns the result back into a host index, so no byte swaps are needed.
    return vi(_mm_shuffle_epi8(iv(a), _mm_xor_si128(_mm_and_si128(_mm_add_epi8(iota_h(), _mm_set1_epi8((char)n)), _mm_set1_epi8(15)), _mm_set1_epi8(3))));
}
// d[i] = i + n < 16 ? s[i + n] : 0   (n in 0..31; pshufb zeroes lanes whose index has bit 7 set)
inline V shl_bytes(const V &a, int n) {
    __m128i idx = _mm_add_epi8(iota_h(), _mm_set1_epi8((char)n));
    idx = _mm_or_si128(idx, _mm_cmpgt_epi8(idx, _mm_set1_epi8(15)));
    return vi(_mm_shuffle_epi8(iv(a), _mm_xor_si128(idx, _mm_set1_epi8(3))));
}
// d[i] = i - n >= 0 ? s[i - n] : 0   (n in 0..31)
inline V shr_bytes(const V &a, int n) {
    __m128i idx = _mm_sub_epi8(iota_h(), _mm_set1_epi8((char)n));
    idx = _mm_or_si128(idx, _mm_cmplt_epi8(idx, _mm_setzero_si128()));
    return vi(_mm_shuffle_epi8(iv(a), _mm_xor_si128(idx, _mm_set1_epi8(3))));
}

// cbd/chd/cwd/cdd: a shufb control that inserts a preferred-slot element of
// `size` bytes at offset t (byte: 03, half: 02 03, word: 00..03, dword: 00..07)
struct InsertMasks { uint32_t m[4][16][4]; };   // size index (1, 2, 4, 8 bytes), aligned offset, words
constexpr InsertMasks make_insert_masks() {
    InsertMasks x{};
    const uint8_t pat[4][8] = {{3}, {2, 3}, {0, 1, 2, 3}, {0, 1, 2, 3, 4, 5, 6, 7}};
    for (int si = 0; si < 4; si++) {
        int size = 1 << si;
        for (int t = 0; t < 16; t++) {
            uint8_t b[16] = {};
            for (int i = 0; i < 16; i++) b[i] = (uint8_t)(0x10 + i);
            int at = t & (0xf & ~(size - 1));
            for (int i = 0; i < size; i++) b[at + i] = pat[si][i];
            for (int w = 0; w < 4; w++)
                x.m[si][t][w] = (uint32_t)b[4 * w] << 24 | (uint32_t)b[4 * w + 1] << 16 | (uint32_t)b[4 * w + 2] << 8 | b[4 * w + 3];
        }
    }
    return x;
}
inline constexpr InsertMasks g_insert_masks = make_insert_masks();
inline V insert_mask(uint32_t t, int size) {
    int si = size == 1 ? 0 : size == 2 ? 1 : size == 4 ? 2 : 3;
    V r;
    memcpy(r.w, g_insert_masks.m[si][t & 0xf], 16);
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
    case IOHL: gpr[rt] = vi(_mm_or_si128(iv(gpr[rt]), _mm_set1_epi32((int)i16))); break;
    case ILA: gpr[rt] = splat((x >> 7) & 0x3ffff); break;
    case FSMBI: for (int i = 0; i < 16; i++) r.set_b(i, (i16 >> (15 - i)) & 1 ? 0xff : 0); gpr[rt] = r; break;

    /* ---------- integer arithmetic ---------- */
    case AH: for (int i = 0; i < 8; i++) r.set_h(i, va.h(i) + vb.h(i)); gpr[rt] = r; break;
    case AHI: for (int i = 0; i < 8; i++) r.set_h(i, va.h(i) + (uint16_t)i10); gpr[rt] = r; break;
    case A: gpr[rt] = vi(_mm_add_epi32(iv(va), iv(vb))); break;
    case AI: gpr[rt] = vi(_mm_add_epi32(iv(va), _mm_set1_epi32((int)i10))); break;
    case SFH: for (int i = 0; i < 8; i++) r.set_h(i, vb.h(i) - va.h(i)); gpr[rt] = r; break;
    case SFHI: for (int i = 0; i < 8; i++) r.set_h(i, (uint16_t)i10 - va.h(i)); gpr[rt] = r; break;
    case SF: gpr[rt] = vi(_mm_sub_epi32(iv(vb), iv(va))); break;
    case SFI: gpr[rt] = vi(_mm_sub_epi32(_mm_set1_epi32((int)i10), iv(va))); break;
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
    case MPY: gpr[rt] = vi(_mm_madd_epi16(lo16(iv(va)), lo16(iv(vb)))); break;
    case MPYU: gpr[rt] = vi(_mm_mullo_epi32(lo16(iv(va)), lo16(iv(vb)))); break;
    case MPYI: gpr[rt] = vi(_mm_madd_epi16(lo16(iv(va)), _mm_set1_epi32(i10 & 0xffff))); break;
    case MPYUI: gpr[rt] = vi(_mm_mullo_epi32(lo16(iv(va)), _mm_set1_epi32(i10 & 0xffff))); break;
    case MPYA: gpr[(x >> 21) & 0x7f] = vi(_mm_add_epi32(_mm_madd_epi16(lo16(iv(va)), lo16(iv(vb))), iv(gpr[rt]))); break;
    case MPYH: gpr[rt] = vi(_mm_slli_epi32(_mm_mullo_epi32(_mm_srli_epi32(iv(va), 16), lo16(iv(vb))), 16)); break;
    case MPYS: gpr[rt] = vi(_mm_srai_epi32(_mm_madd_epi16(lo16(iv(va)), lo16(iv(vb))), 16)); break;
    case MPYHH: gpr[rt] = vi(_mm_madd_epi16(_mm_srli_epi32(iv(va), 16), _mm_srli_epi32(iv(vb), 16))); break;
    case MPYHHA: for (int i = 0; i < 4; i++) r.w[i] = gpr[rt].w[i] + (uint32_t)((int16_t)(va.w[i] >> 16) * (int16_t)(vb.w[i] >> 16)); gpr[rt] = r; break;
    case MPYHHU: gpr[rt] = vi(_mm_mullo_epi32(_mm_srli_epi32(iv(va), 16), _mm_srli_epi32(iv(vb), 16))); break;
    case MPYHHAU: for (int i = 0; i < 4; i++) r.w[i] = gpr[rt].w[i] + (va.w[i] >> 16) * (vb.w[i] >> 16); gpr[rt] = r; break;
    case CLZ: for (int i = 0; i < 4; i++) r.w[i] = va.w[i] ? __builtin_clz(va.w[i]) : 32; gpr[rt] = r; break;
    case CNTB: for (int i = 0; i < 16; i++) r.set_b(i, (uint8_t)__builtin_popcount(va.b(i))); gpr[rt] = r; break;
    case FSMB: for (int i = 0; i < 16; i++) r.set_b(i, (va.w[0] >> (15 - i)) & 1 ? 0xff : 0); gpr[rt] = r; break;
    case FSMH: for (int i = 0; i < 8; i++) r.set_h(i, (va.w[0] >> (7 - i)) & 1 ? 0xffff : 0); gpr[rt] = r; break;
    case FSM: { const __m128i bits = _mm_setr_epi32(8, 4, 2, 1); gpr[rt] = vi(_mm_cmpeq_epi32(_mm_and_si128(_mm_set1_epi32((int)va.w[0]), bits), bits)); break; }
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
    case XSBH: gpr[rt] = vi(_mm_srai_epi16(_mm_slli_epi16(iv(va), 8), 8)); break;
    case XSHW: gpr[rt] = vi(_mm_srai_epi32(_mm_slli_epi32(iv(va), 16), 16)); break;
    case XSWD: for (int i = 0; i < 2; i++) r.set_d(i, (uint64_t)(int64_t)(int32_t)va.w[2 * i + 1]); gpr[rt] = r; break;

    /* ---------- logical ---------- */
    case AND: gpr[rt] = vi(_mm_and_si128(iv(va), iv(vb))); break;
    case ANDC: gpr[rt] = vi(_mm_andnot_si128(iv(vb), iv(va))); break;
    case ANDBI: gpr[rt] = vi(_mm_and_si128(iv(va), _mm_set1_epi8((char)i10))); break;
    case ANDHI: for (int i = 0; i < 8; i++) r.set_h(i, va.h(i) & (uint16_t)i10); gpr[rt] = r; break;
    case ANDI: gpr[rt] = vi(_mm_and_si128(iv(va), _mm_set1_epi32((int)i10))); break;
    case OR: gpr[rt] = vi(_mm_or_si128(iv(va), iv(vb))); break;
    case ORC: gpr[rt] = vi(_mm_or_si128(iv(va), not_(iv(vb)))); break;
    case ORBI: gpr[rt] = vi(_mm_or_si128(iv(va), _mm_set1_epi8((char)i10))); break;
    case ORHI: for (int i = 0; i < 8; i++) r.set_h(i, va.h(i) | (uint16_t)i10); gpr[rt] = r; break;
    case ORI: gpr[rt] = vi(_mm_or_si128(iv(va), _mm_set1_epi32((int)i10))); break;
    case ORX: r.w[0] = va.w[0] | va.w[1] | va.w[2] | va.w[3]; gpr[rt] = r; break;
    case XOR: gpr[rt] = vi(_mm_xor_si128(iv(va), iv(vb))); break;
    case XORBI: for (int i = 0; i < 16; i++) r.set_b(i, va.b(i) ^ (uint8_t)i10); gpr[rt] = r; break;
    case XORHI: for (int i = 0; i < 8; i++) r.set_h(i, va.h(i) ^ (uint16_t)i10); gpr[rt] = r; break;
    case XORI: gpr[rt] = vi(_mm_xor_si128(iv(va), _mm_set1_epi32((int)i10))); break;
    case NAND: gpr[rt] = vi(not_(_mm_and_si128(iv(va), iv(vb)))); break;
    case NOR: gpr[rt] = vi(not_(_mm_or_si128(iv(va), iv(vb)))); break;
    case EQV: gpr[rt] = vi(not_(_mm_xor_si128(iv(va), iv(vb)))); break;
    case SELB: gpr[(x >> 21) & 0x7f] = vi(_mm_or_si128(_mm_and_si128(iv(gpr[rt]), iv(vb)), _mm_andnot_si128(iv(gpr[rt]), iv(va)))); break;
    case SHUFB: {
        // Selector byte c: 0x00-0x1f picks byte c of a:b, 10xxxxxx gives 0x00, 110xxxxx 0xff,
        // 111xxxxx 0x80. Two pshufb lookups (a, b) blended on bit 4, then the constants.
        // Registers hold each word in host order, so big-endian byte n is host byte n ^ 3 and
        // the lookup needs no byte swaps: the index is just c ^ 3.
        __m128i A = iv(va), B = iv(vb), C = iv(gpr[rt]);
#if defined(__AVX512VBMI__) && defined(__AVX512VL__)
        __m128i sel = _mm_permutex2var_epi8(A, _mm_xor_si128(C, _mm_set1_epi8(3)), B);   // low 5 bits: 16 bytes of A, then B
#else
        __m128i idx = _mm_and_si128(_mm_xor_si128(C, _mm_set1_epi8(3)), _mm_set1_epi8(0x0f));
        __m128i bit4 = _mm_cmpeq_epi8(_mm_and_si128(C, _mm_set1_epi8(0x10)), _mm_set1_epi8(0x10));
        __m128i sel = _mm_blendv_epi8(_mm_shuffle_epi8(A, idx), _mm_shuffle_epi8(B, idx), bit4);
#endif
        __m128i hi = _mm_cmplt_epi8(C, _mm_setzero_si128());   // c >= 0x80
        __m128i c0 = _mm_cmpeq_epi8(_mm_and_si128(C, _mm_set1_epi8((char)0xc0)), _mm_set1_epi8((char)0xc0));
        __m128i e0 = _mm_cmpeq_epi8(_mm_and_si128(C, _mm_set1_epi8((char)0xe0)), _mm_set1_epi8((char)0xe0));
        __m128i k = _mm_blendv_epi8(_mm_and_si128(c0, _mm_set1_epi8((char)0xff)), _mm_set1_epi8((char)0x80), e0);
        gpr[(x >> 21) & 0x7f] = vi(_mm_blendv_epi8(sel, k, hi));
        break;
    }

    /* ---------- shifts and rotates ---------- */
    case SHLH: for (int i = 0; i < 8; i++) { int s = vb.h(i) & 0x1f; r.set_h(i, s > 15 ? 0 : (uint16_t)(va.h(i) << s)); } gpr[rt] = r; break;
    case SHLHI: { int s = i7 & 0x1f; for (int i = 0; i < 8; i++) r.set_h(i, s > 15 ? 0 : (uint16_t)(va.h(i) << s)); gpr[rt] = r; break; }
    case SHL: gpr[rt] = vi(_mm_sllv_epi32(iv(va), _mm_and_si128(iv(vb), _mm_set1_epi32(0x3f)))); break;
    case SHLI: gpr[rt] = vi(_mm_sll_epi32(iv(va), _mm_cvtsi32_si128(i7 & 0x3f))); break;
    case ROTH: for (int i = 0; i < 8; i++) { int s = vb.h(i) & 0xf; uint16_t v = va.h(i); r.set_h(i, (uint16_t)((v << s) | (v >> ((16 - s) & 15)))); } gpr[rt] = r; break;
    case ROTHI: { int s = i7 & 0xf; for (int i = 0; i < 8; i++) { uint16_t v = va.h(i); r.set_h(i, (uint16_t)((v << s) | (v >> ((16 - s) & 15)))); } gpr[rt] = r; break; }
    case ROT: { __m128i n = _mm_and_si128(iv(vb), _mm_set1_epi32(0x1f)); gpr[rt] = vi(_mm_or_si128(_mm_sllv_epi32(iv(va), n), _mm_srlv_epi32(iv(va), _mm_sub_epi32(_mm_set1_epi32(32), n)))); } break;
    case ROTI: { int n = i7 & 0x1f; gpr[rt] = vi(_mm_or_si128(_mm_sll_epi32(iv(va), _mm_cvtsi32_si128(n)), _mm_srl_epi32(iv(va), _mm_cvtsi32_si128(32 - n)))); } break;
    case ROTHM: for (int i = 0; i < 8; i++) { int s = (0 - vb.h(i)) & 0x1f; r.set_h(i, s > 15 ? 0 : (uint16_t)(va.h(i) >> s)); } gpr[rt] = r; break;
    case ROTHMI: { int s = (0 - i7) & 0x1f; for (int i = 0; i < 8; i++) r.set_h(i, s > 15 ? 0 : (uint16_t)(va.h(i) >> s)); gpr[rt] = r; break; }
    case ROTM: gpr[rt] = vi(_mm_srlv_epi32(iv(va), _mm_and_si128(_mm_sub_epi32(_mm_setzero_si128(), iv(vb)), _mm_set1_epi32(0x3f)))); break;
    case ROTMI: gpr[rt] = vi(_mm_srl_epi32(iv(va), _mm_cvtsi32_si128((0 - i7) & 0x3f))); break;
    case ROTMAH: for (int i = 0; i < 8; i++) { int s = (0 - vb.h(i)) & 0x1f; r.set_h(i, (uint16_t)((int16_t)va.h(i) >> (s > 15 ? 15 : s))); } gpr[rt] = r; break;
    case ROTMAHI: { int s = (0 - i7) & 0x1f; for (int i = 0; i < 8; i++) r.set_h(i, (uint16_t)((int16_t)va.h(i) >> (s > 15 ? 15 : s))); gpr[rt] = r; break; }
    case ROTMA: gpr[rt] = vi(_mm_srav_epi32(iv(va), _mm_and_si128(_mm_sub_epi32(_mm_setzero_si128(), iv(vb)), _mm_set1_epi32(0x3f)))); break;
    case ROTMAI: gpr[rt] = vi(_mm_sra_epi32(iv(va), _mm_cvtsi32_si128((0 - i7) & 0x3f))); break;
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
    case CEQB: gpr[rt] = vi(_mm_cmpeq_epi8(iv(va), iv(vb))); break;
    case CEQBI: gpr[rt] = vi(_mm_cmpeq_epi8(iv(va), _mm_set1_epi8((char)i10))); break;
    case CEQH: gpr[rt] = vi(_mm_cmpeq_epi16(iv(va), iv(vb))); break;
    case CEQHI: gpr[rt] = vi(_mm_cmpeq_epi16(iv(va), _mm_set1_epi16((short)i10))); break;
    case CEQ: gpr[rt] = vi(_mm_cmpeq_epi32(iv(va), iv(vb))); break;
    case CEQI: gpr[rt] = vi(_mm_cmpeq_epi32(iv(va), _mm_set1_epi32((int)i10))); break;
    case CGTB: gpr[rt] = vi(_mm_cmpgt_epi8(iv(va), iv(vb))); break;
    case CGTBI: gpr[rt] = vi(_mm_cmpgt_epi8(iv(va), _mm_set1_epi8((char)i10))); break;
    case CGTH: for (int i = 0; i < 8; i++) r.set_h(i, (int16_t)va.h(i) > (int16_t)vb.h(i) ? 0xffff : 0); gpr[rt] = r; break;
    case CGTHI: for (int i = 0; i < 8; i++) r.set_h(i, (int16_t)va.h(i) > (int16_t)i10 ? 0xffff : 0); gpr[rt] = r; break;
    case CGT: gpr[rt] = vi(_mm_cmpgt_epi32(iv(va), iv(vb))); break;
    case CGTI: gpr[rt] = vi(_mm_cmpgt_epi32(iv(va), _mm_set1_epi32((int)i10))); break;
    case CLGTB: gpr[rt] = vi(_mm_xor_si128(_mm_cmpeq_epi8(_mm_max_epu8(iv(vb), iv(va)), iv(vb)), _mm_set1_epi8(-1))); break;
    case CLGTBI: gpr[rt] = vi(_mm_xor_si128(_mm_cmpeq_epi8(_mm_max_epu8(_mm_set1_epi8((char)i10), iv(va)), _mm_set1_epi8((char)i10)), _mm_set1_epi8(-1))); break;
    case CLGTH: for (int i = 0; i < 8; i++) r.set_h(i, va.h(i) > vb.h(i) ? 0xffff : 0); gpr[rt] = r; break;
    case CLGTHI: for (int i = 0; i < 8; i++) r.set_h(i, va.h(i) > (uint16_t)i10 ? 0xffff : 0); gpr[rt] = r; break;
    case CLGT: gpr[rt] = vi(cmpgt_u32(iv(va), iv(vb))); break;
    case CLGTI: gpr[rt] = vi(cmpgt_u32(iv(va), _mm_set1_epi32((int)i10))); break;

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
    case FA: gpr[rt] = fv(f_add(vf(va), vf(vb))); break;
    case FS: gpr[rt] = fv(f_sub(vf(va), vf(vb))); break;
    case FM: gpr[rt] = fv(f_mul(vf(va), vf(vb))); break;
    case FMA: gpr[(x >> 21) & 0x7f] = fv(f_madd(vf(va), vf(vb), vf(gpr[rt]))); break;
    case FMS: gpr[(x >> 21) & 0x7f] = fv(f_msub(vf(va), vf(vb), vf(gpr[rt]))); break;
    case FNMS: gpr[(x >> 21) & 0x7f] = fv(f_nmsub(vf(va), vf(vb), vf(gpr[rt]))); break;
    // frest/frsqest return the exact value; fi then passes it through.
    case FREST: gpr[rt] = fv(v_spr(_mm_div_ps(_mm_set1_ps(1.0f), v_spf(vf(va))))); break;
    case FRSQEST: gpr[rt] = fv(v_spr(_mm_div_ps(_mm_set1_ps(1.0f), _mm_sqrt_ps(_mm_andnot_ps(_mm_set1_ps(-0.0f), v_spf(vf(va))))))); break;
    case FI: gpr[rt] = vb; break;
    case FCEQ: gpr[rt] = fv(_mm_cmpeq_ps(v_ftz(vf(va)), v_ftz(vf(vb)))); break;
    case FCMEQ: for (int i = 0; i < 4; i++) r.w[i] = std::fabs(ftz(va.f(i))) == std::fabs(ftz(vb.f(i))) ? ~0u : 0; gpr[rt] = r; break;
    case FCGT: gpr[rt] = fv(_mm_cmpgt_ps(v_ftz(vf(va)), v_ftz(vf(vb)))); break;
    case FCMGT: for (int i = 0; i < 4; i++) r.w[i] = std::fabs(ftz(va.f(i))) > std::fabs(ftz(vb.f(i))) ? ~0u : 0; gpr[rt] = r; break;
    case CSFLT: {
        int sc = 155 - (int)((x >> 14) & 0xff);
        // int -> float rounds once; scaling by 2^-sc is then exact unless the result leaves
        // the normal range (|sc| small enough here) - the same single rounding as via double.
        if (sc >= 0 && sc <= 96) {
            gpr[rt] = fv(_mm_mul_ps(_mm_cvtepi32_ps(_mm_loadu_si128((const __m128i *)va.w)), _mm_set1_ps((float)pow2(-sc))));
            break;
        }
        for (int i = 0; i < 4; i++) r.set_f(i, (float)((double)(int32_t)va.w[i] * pow2(-sc)));
        gpr[rt] = r;
        break;
    }
    case CUFLT: { int sc = 155 - (int)((x >> 14) & 0xff); for (int i = 0; i < 4; i++) r.set_f(i, (float)((double)va.w[i] * pow2(-sc))); gpr[rt] = r; break; }
    case CFLTS: {
        int sc = 173 - (int)((x >> 14) & 0xff);
        // f * 2^sc in float is exact unless it overflows (saturates anyway) or underflows
        // (truncates to 0 anyway); then truncate with saturation: NaN -> 0, out of range -> min/max.
        if (sc >= 0 && sc <= 64) {
            __m128 f = _mm_mul_ps(vf(va), _mm_set1_ps((float)pow2(sc)));
            __m128i t = _mm_cvttps_epi32(f);   // 0x80000000 when out of range or NaN
            __m128i pos = _mm_castps_si128(_mm_cmpge_ps(f, _mm_set1_ps(2147483648.0f)));
            __m128i nan = _mm_castps_si128(_mm_cmpunord_ps(f, f));
            t = _mm_blendv_epi8(t, _mm_set1_epi32(0x7fffffff), pos);
            t = _mm_andnot_si128(nan, t);
            V o;
            _mm_storeu_si128((__m128i *)o.w, t);
            gpr[rt] = o;
            break;
        }
        for (int i = 0; i < 4; i++) r.w[i] = (uint32_t)sat_s32((double)va.f(i) * pow2(sc));
        gpr[rt] = r;
        break;
    }
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
