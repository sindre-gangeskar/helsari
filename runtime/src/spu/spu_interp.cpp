// SPU interpreter: decodes and executes the full SPU instruction set.
// Registers are V (big-endian slot order, w[0] = preferred slot); see spu.h.
#include "spu.h"

#include "runtime.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <map>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace spu {
namespace {

enum Op : uint8_t {
    INVALID,
    // RR / RI7 (11-bit opcode)
    STOP, LNOP, SYNC, DSYNC, MFSPR, RDCH, RCHCNT, SF, OR, BG, SFH, NOR, ABSDB, ROT, ROTM, ROTMA, SHL,
    ROTH, ROTHM, ROTMAH, SHLH, ROTI, ROTMI, ROTMAI, SHLI, ROTHI, ROTHMI, ROTMAHI, SHLHI, A, AND, CG,
    AH, NAND, AVGB, MTSPR, WRCH, BIZ, BINZ, BIHZ, BIHNZ, STOPD, STQX, BI, BISL, IRET, BISLED, HBR, GB,
    GBH, GBB, FSM, FSMH, FSMB, FREST, FRSQEST, LQX, ROTQBYBI, ROTQMBYBI, SHLQBYBI, CBX, CHX, CWX, CDX,
    ROTQBI, ROTQMBI, SHLQBI, ROTQBY, ROTQMBY, SHLQBY, ORX, CBD, CHD, CWD, CDD, ROTQBII, ROTQMBII,
    SHLQBII, ROTQBYI, ROTQMBYI, SHLQBYI, NOP, CGT, XOR, CGTH, EQV, CGTB, SUMB, HGT, CLZ, XSWD, XSHW,
    CNTB, XSBH, CLGT, ANDC, FCGT, DFCGT, FA, FS, FM, CLGTH, ORC, FCMGT, DFCMGT, DFA, DFS, DFM, CLGTB,
    HLGT, DFMA, DFMS, DFNMS, DFNMA, CEQ, MPYHHU, ADDX, SFX, CGX, BGX, MPYHHA, MPYHHAU, FSCRRD, FESD,
    FRDS, FSCRWR, DFTSV, FCEQ, DFCEQ, MPY, MPYH, MPYHH, MPYS, CEQH, FCMEQ, DFCMEQ, MPYU, CEQB, FI, HEQ,
    // RI8 (10-bit)
    CFLTS, CFLTU, CSFLT, CUFLT,
    // RI10 (8-bit)
    ORI, ORHI, ORBI, SFI, SFHI, ANDI, ANDHI, ANDBI, AI, AHI, STQD, LQD, XORI, XORHI, XORBI, CGTI, CGTHI,
    CGTBI, HGTI, CLGTI, CLGTHI, CLGTBI, HLGTI, MPYI, MPYUI, CEQI, CEQHI, CEQBI, HEQI,
    // RI16 (9-bit)
    BRZ, STQA, BRNZ, BRHZ, BRHNZ, STQR, BRA, LQA, BRASL, BR, FSMBI, BRSL, LQR, IL, ILHU, ILH, IOHL,
    // RI18 (7-bit)
    HBRA, HBRR, ILA,
    // RRR (4-bit)
    SELB, SHUFB, MPYA, FNMS, FMA, FMS,
    OP_COUNT
};

struct OpInfo { const char *name; int bits; uint32_t code; char fmt; };
// fmt: R = rt,ra,rb  I = rt,ra,i7  T = rt,ra,i10  S = rt,i16  L = rt,i18  Q = rt,ra,rb,rc
//      8 = rt,ra,i8  N = none  B = branch i16  C = channel
const OpInfo k_ops[] = {
    {"?", 0, 0, 'N'},
    {"stop", 11, 0x000, 'N'}, {"lnop", 11, 0x001, 'N'}, {"sync", 11, 0x002, 'N'}, {"dsync", 11, 0x003, 'N'},
    {"mfspr", 11, 0x00c, 'R'}, {"rdch", 11, 0x00d, 'C'}, {"rchcnt", 11, 0x00f, 'C'}, {"sf", 11, 0x040, 'R'},
    {"or", 11, 0x041, 'R'}, {"bg", 11, 0x042, 'R'}, {"sfh", 11, 0x048, 'R'}, {"nor", 11, 0x049, 'R'},
    {"absdb", 11, 0x053, 'R'}, {"rot", 11, 0x058, 'R'}, {"rotm", 11, 0x059, 'R'}, {"rotma", 11, 0x05a, 'R'},
    {"shl", 11, 0x05b, 'R'}, {"roth", 11, 0x05c, 'R'}, {"rothm", 11, 0x05d, 'R'}, {"rotmah", 11, 0x05e, 'R'},
    {"shlh", 11, 0x05f, 'R'}, {"roti", 11, 0x078, 'I'}, {"rotmi", 11, 0x079, 'I'}, {"rotmai", 11, 0x07a, 'I'},
    {"shli", 11, 0x07b, 'I'}, {"rothi", 11, 0x07c, 'I'}, {"rothmi", 11, 0x07d, 'I'}, {"rotmahi", 11, 0x07e, 'I'},
    {"shlhi", 11, 0x07f, 'I'}, {"a", 11, 0x0c0, 'R'}, {"and", 11, 0x0c1, 'R'}, {"cg", 11, 0x0c2, 'R'},
    {"ah", 11, 0x0c8, 'R'}, {"nand", 11, 0x0c9, 'R'}, {"avgb", 11, 0x0d3, 'R'}, {"mtspr", 11, 0x10c, 'R'},
    {"wrch", 11, 0x10d, 'C'}, {"biz", 11, 0x128, 'R'}, {"binz", 11, 0x129, 'R'}, {"bihz", 11, 0x12a, 'R'},
    {"bihnz", 11, 0x12b, 'R'}, {"stopd", 11, 0x140, 'N'}, {"stqx", 11, 0x144, 'R'}, {"bi", 11, 0x1a8, 'R'},
    {"bisl", 11, 0x1a9, 'R'}, {"iret", 11, 0x1aa, 'N'}, {"bisled", 11, 0x1ab, 'R'}, {"hbr", 11, 0x1ac, 'N'},
    {"gb", 11, 0x1b0, 'R'}, {"gbh", 11, 0x1b1, 'R'}, {"gbb", 11, 0x1b2, 'R'}, {"fsm", 11, 0x1b4, 'R'},
    {"fsmh", 11, 0x1b5, 'R'}, {"fsmb", 11, 0x1b6, 'R'}, {"frest", 11, 0x1b8, 'R'}, {"frsqest", 11, 0x1b9, 'R'},
    {"lqx", 11, 0x1c4, 'R'}, {"rotqbybi", 11, 0x1cc, 'R'}, {"rotqmbybi", 11, 0x1cd, 'R'},
    {"shlqbybi", 11, 0x1cf, 'R'}, {"cbx", 11, 0x1d4, 'R'}, {"chx", 11, 0x1d5, 'R'}, {"cwx", 11, 0x1d6, 'R'},
    {"cdx", 11, 0x1d7, 'R'}, {"rotqbi", 11, 0x1d8, 'R'}, {"rotqmbi", 11, 0x1d9, 'R'}, {"shlqbi", 11, 0x1db, 'R'},
    {"rotqby", 11, 0x1dc, 'R'}, {"rotqmby", 11, 0x1dd, 'R'}, {"shlqby", 11, 0x1df, 'R'}, {"orx", 11, 0x1f0, 'R'},
    {"cbd", 11, 0x1f4, 'I'}, {"chd", 11, 0x1f5, 'I'}, {"cwd", 11, 0x1f6, 'I'}, {"cdd", 11, 0x1f7, 'I'},
    {"rotqbii", 11, 0x1f8, 'I'}, {"rotqmbii", 11, 0x1f9, 'I'}, {"shlqbii", 11, 0x1fb, 'I'},
    {"rotqbyi", 11, 0x1fc, 'I'}, {"rotqmbyi", 11, 0x1fd, 'I'}, {"shlqbyi", 11, 0x1ff, 'I'}, {"nop", 11, 0x201, 'N'},
    {"cgt", 11, 0x240, 'R'}, {"xor", 11, 0x241, 'R'}, {"cgth", 11, 0x248, 'R'}, {"eqv", 11, 0x249, 'R'},
    {"cgtb", 11, 0x250, 'R'}, {"sumb", 11, 0x253, 'R'}, {"hgt", 11, 0x258, 'R'}, {"clz", 11, 0x2a5, 'R'},
    {"xswd", 11, 0x2a6, 'R'}, {"xshw", 11, 0x2ae, 'R'}, {"cntb", 11, 0x2b4, 'R'}, {"xsbh", 11, 0x2b6, 'R'},
    {"clgt", 11, 0x2c0, 'R'}, {"andc", 11, 0x2c1, 'R'}, {"fcgt", 11, 0x2c2, 'R'}, {"dfcgt", 11, 0x2c3, 'R'},
    {"fa", 11, 0x2c4, 'R'}, {"fs", 11, 0x2c5, 'R'}, {"fm", 11, 0x2c6, 'R'}, {"clgth", 11, 0x2c8, 'R'},
    {"orc", 11, 0x2c9, 'R'}, {"fcmgt", 11, 0x2ca, 'R'}, {"dfcmgt", 11, 0x2cb, 'R'}, {"dfa", 11, 0x2cc, 'R'},
    {"dfs", 11, 0x2cd, 'R'}, {"dfm", 11, 0x2ce, 'R'}, {"clgtb", 11, 0x2d0, 'R'}, {"hlgt", 11, 0x2d8, 'R'},
    {"dfma", 11, 0x35c, 'R'}, {"dfms", 11, 0x35d, 'R'}, {"dfnms", 11, 0x35e, 'R'}, {"dfnma", 11, 0x35f, 'R'},
    {"ceq", 11, 0x3c0, 'R'}, {"mpyhhu", 11, 0x3ce, 'R'}, {"addx", 11, 0x340, 'R'}, {"sfx", 11, 0x341, 'R'},
    {"cgx", 11, 0x342, 'R'}, {"bgx", 11, 0x343, 'R'}, {"mpyhha", 11, 0x346, 'R'}, {"mpyhhau", 11, 0x34e, 'R'},
    {"fscrrd", 11, 0x398, 'R'}, {"fesd", 11, 0x3b8, 'R'}, {"frds", 11, 0x3b9, 'R'}, {"fscrwr", 11, 0x3ba, 'R'},
    {"dftsv", 11, 0x3bf, 'R'}, {"fceq", 11, 0x3c2, 'R'}, {"dfceq", 11, 0x3c3, 'R'}, {"mpy", 11, 0x3c4, 'R'},
    {"mpyh", 11, 0x3c5, 'R'}, {"mpyhh", 11, 0x3c6, 'R'}, {"mpys", 11, 0x3c7, 'R'}, {"ceqh", 11, 0x3c8, 'R'},
    {"fcmeq", 11, 0x3ca, 'R'}, {"dfcmeq", 11, 0x3cb, 'R'}, {"mpyu", 11, 0x3cc, 'R'}, {"ceqb", 11, 0x3d0, 'R'},
    {"fi", 11, 0x3d4, 'R'}, {"heq", 11, 0x3d8, 'R'},
    {"cflts", 10, 0x1d8, '8'}, {"cfltu", 10, 0x1d9, '8'}, {"csflt", 10, 0x1da, '8'}, {"cuflt", 10, 0x1db, '8'},
    {"ori", 8, 0x04, 'T'}, {"orhi", 8, 0x05, 'T'}, {"orbi", 8, 0x06, 'T'}, {"sfi", 8, 0x0c, 'T'},
    {"sfhi", 8, 0x0d, 'T'}, {"andi", 8, 0x14, 'T'}, {"andhi", 8, 0x15, 'T'}, {"andbi", 8, 0x16, 'T'},
    {"ai", 8, 0x1c, 'T'}, {"ahi", 8, 0x1d, 'T'}, {"stqd", 8, 0x24, 'T'}, {"lqd", 8, 0x34, 'T'},
    {"xori", 8, 0x44, 'T'}, {"xorhi", 8, 0x45, 'T'}, {"xorbi", 8, 0x46, 'T'}, {"cgti", 8, 0x4c, 'T'},
    {"cgthi", 8, 0x4d, 'T'}, {"cgtbi", 8, 0x4e, 'T'}, {"hgti", 8, 0x4f, 'T'}, {"clgti", 8, 0x5c, 'T'},
    {"clgthi", 8, 0x5d, 'T'}, {"clgtbi", 8, 0x5e, 'T'}, {"hlgti", 8, 0x5f, 'T'}, {"mpyi", 8, 0x74, 'T'},
    {"mpyui", 8, 0x75, 'T'}, {"ceqi", 8, 0x7c, 'T'}, {"ceqhi", 8, 0x7d, 'T'}, {"ceqbi", 8, 0x7e, 'T'},
    {"heqi", 8, 0x7f, 'T'},
    {"brz", 9, 0x40, 'B'}, {"stqa", 9, 0x41, 'S'}, {"brnz", 9, 0x42, 'B'}, {"brhz", 9, 0x44, 'B'},
    {"brhnz", 9, 0x46, 'B'}, {"stqr", 9, 0x47, 'B'}, {"bra", 9, 0x60, 'S'}, {"lqa", 9, 0x61, 'S'},
    {"brasl", 9, 0x62, 'S'}, {"br", 9, 0x64, 'B'}, {"fsmbi", 9, 0x65, 'S'}, {"brsl", 9, 0x66, 'B'},
    {"lqr", 9, 0x67, 'B'}, {"il", 9, 0x81, 'S'}, {"ilhu", 9, 0x82, 'S'}, {"ilh", 9, 0x83, 'S'},
    {"iohl", 9, 0xc1, 'S'},
    {"hbra", 7, 0x08, 'N'}, {"hbrr", 7, 0x09, 'N'}, {"ila", 7, 0x21, 'L'},
    {"selb", 4, 0x8, 'Q'}, {"shufb", 4, 0xb, 'Q'}, {"mpya", 4, 0xc, 'Q'}, {"fnms", 4, 0xd, 'Q'},
    {"fma", 4, 0xe, 'Q'}, {"fms", 4, 0xf, 'Q'},
};
static_assert(sizeof(k_ops) / sizeof(k_ops[0]) == OP_COUNT, "opcode table out of sync with Op");

// Decode table indexed by the top 11 bits of the instruction.
struct Decoder {
    uint8_t t[2048] = {};
    Decoder() {
        // Narrower opcodes first so exact 11-bit entries win.
        for (int bits : {4, 7, 8, 9, 10, 11})
            for (int op = 1; op < OP_COUNT; op++)
                if (k_ops[op].bits == bits) {
                    int span = 1 << (11 - bits);
                    for (int i = 0; i < span; i++) t[(k_ops[op].code << (11 - bits)) | i] = (uint8_t)op;
                }
    }
};
const Decoder g_dec;

inline Op decode(uint32_t x) { return (Op)g_dec.t[x >> 21]; }
inline int32_t sext(uint32_t v, int bits) { return (int32_t)(v << (32 - bits)) >> (32 - bits); }

// Flush single-precision denormals to zero, as the SPU does.
inline float ftz(float f) { return std::fpclassify(f) == FP_SUBNORMAL ? std::copysign(0.0f, f) : f; }

V splat(uint32_t v) { return V{{v, v, v, v}}; }

// 128-bit helpers on the big-endian register image.
// Big-endian byte i of a register is host byte i ^ 3: one shuffle converts either way.
void to_bytes(const V &v, uint8_t *o) {
    _mm_storeu_si128((__m128i *)o, _mm_shuffle_epi8(_mm_loadu_si128((const __m128i *)v.w), SpuThread::bswap32_mask()));
}
V from_bytes(const uint8_t *o) {
    V r;
    _mm_storeu_si128((__m128i *)r.w, _mm_shuffle_epi8(_mm_loadu_si128((const __m128i *)o), SpuThread::bswap32_mask()));
    return r;
}
V shl_bits(const V &a, int s) {   // s in 0..7
    if (!s) return a;
    V r;
    for (int i = 0; i < 4; i++) r.w[i] = (a.w[i] << s) | (i < 3 ? a.w[i + 1] >> (32 - s) : 0);
    return r;
}
V shr_bits(const V &a, int s) {   // s in 0..7
    if (!s) return a;
    V r;
    for (int i = 0; i < 4; i++) r.w[i] = (a.w[i] >> s) | (i > 0 ? a.w[i - 1] << (32 - s) : 0);
    return r;
}
V rot_bits(const V &a, int s) {   // s in 0..7
    if (!s) return a;
    V r;
    for (int i = 0; i < 4; i++) r.w[i] = (a.w[i] << s) | (a.w[(i + 1) & 3] >> (32 - s));
    return r;
}
V rot_bytes(const V &a, int n) { uint8_t s[16], d[16]; to_bytes(a, s); for (int i = 0; i < 16; i++) d[i] = s[(i + n) & 15]; return from_bytes(d); }
V shl_bytes(const V &a, int n) { uint8_t s[16], d[16]; to_bytes(a, s); for (int i = 0; i < 16; i++) d[i] = i + n < 16 ? s[i + n] : 0; return from_bytes(d); }
V shr_bytes(const V &a, int n) { uint8_t s[16], d[16]; to_bytes(a, s); for (int i = 0; i < 16; i++) d[i] = i - n >= 0 ? s[i - n] : 0; return from_bytes(d); }

// cbd/chd/cwd/cdd: a shufb control that inserts a preferred-slot element of
// `size` bytes at offset t (byte: 03, half: 02 03, word: 00..03, dword: 00..07)
V insert_mask(uint32_t t, int size) {
    V r{{0x10111213u, 0x14151617u, 0x18191a1bu, 0x1c1d1e1fu}};
    t &= 0xf & ~(size - 1);
    static const uint8_t pat1[1] = {3}, pat2[2] = {2, 3}, pat4[4] = {0, 1, 2, 3}, pat8[8] = {0, 1, 2, 3, 4, 5, 6, 7};
    const uint8_t *p = size == 1 ? pat1 : size == 2 ? pat2 : size == 4 ? pat4 : pat8;
    for (int i = 0; i < size; i++) r.set_b(t + i, p[i]);
    return r;
}

int32_t sat_s32(double v) { return v >= 2147483647.0 ? 0x7fffffff : v <= -2147483648.0 ? (int32_t)0x80000000 : std::isnan(v) ? 0 : (int32_t)v; }
uint32_t sat_u32(double v) { return v >= 4294967295.0 ? 0xffffffffu : v <= 0 || std::isnan(v) ? 0 : (uint32_t)v; }

} // namespace

std::string disasm(uint32_t pc, uint32_t x) {
    Op op = decode(x);
    const OpInfo &o = k_ops[op];
    uint32_t rt = x & 0x7f, ra = (x >> 7) & 0x7f, rb = (x >> 14) & 0x7f;
    char buf[96];
    switch (o.fmt) {
    case 'R': snprintf(buf, sizeof buf, "%s r%u,r%u,r%u", o.name, rt, ra, rb); break;
    case 'I': snprintf(buf, sizeof buf, "%s r%u,r%u,%d", o.name, rt, ra, sext(rb, 7)); break;
    case 'T': snprintf(buf, sizeof buf, "%s r%u,r%u,%d", o.name, rt, ra, sext((x >> 14) & 0x3ff, 10)); break;
    case '8': snprintf(buf, sizeof buf, "%s r%u,r%u,%u", o.name, rt, ra, (x >> 14) & 0xff); break;
    case 'S':   // absolute LS forms take a word address; constants show the immediate
        if (op == LQA || op == STQA || op == BRA || op == BRASL)
            snprintf(buf, sizeof buf, "%s r%u,0x%x", o.name, rt, ((x >> 7) & 0xffff) << 2 & (LS_SIZE - 4));
        else
            snprintf(buf, sizeof buf, "%s r%u,0x%x", o.name, rt, (x >> 7) & 0xffff);
        break;
    case 'B': snprintf(buf, sizeof buf, "%s r%u,0x%x", o.name, rt, (pc + (sext((x >> 7) & 0xffff, 16) << 2)) & (LS_SIZE - 4)); break;
    case 'L': snprintf(buf, sizeof buf, "%s r%u,0x%x", o.name, rt, (x >> 7) & 0x3ffff); break;
    case 'Q': snprintf(buf, sizeof buf, "%s r%u,r%u,r%u,r%u", o.name, (x >> 21) & 0x7f, ra, rb, rt); break;
    case 'C': snprintf(buf, sizeof buf, "%s r%u,ch%u", o.name, rt, ra); break;
    default: snprintf(buf, sizeof buf, "%s", o.name); break;
    }
    return buf;
}

void SpuThread::reset() {
    memset(gpr, 0, sizeof gpr);
    pc = 0, srr0 = 0, atomic_stat = 0, raddr = 0, event_mask = 0, event_stat = 0;
    tag_mask = 0, tag_update = 0;
}

void SpuThread::dump_history() {
    LOG("spu", "%s: last instructions before 0x%x:", name.c_str(), pc);
    for (uint32_t i = 0; i < 64; i++) {
        uint32_t a = history[(history_pos + i) & 63];
        LOG("spu", "  %05x: %08x  %s", a, ls32(a), disasm(a, ls32(a)).c_str());
    }
    std::string regs;
    for (int r = 0; r < 8; r++) { char b[24]; snprintf(b, sizeof b, " r%d=%x", r, gpr[r].w[0]); regs += b; }
    LOG("spu", "%s:%s", name.c_str(), regs.c_str());
}

std::atomic<uint64_t> g_insns{0}, g_run_ns{0}, g_wait_ns{0};

// KZ2_SPU_HOTSPOTS=1: every 1024th instruction is sampled as (thread, pc, hash of
// the 256 code bytes around it - job managers load different code at the same
// LS address); stats_line dumps the top samples every 6th call.
namespace {
struct HotSample { uint64_t count = 0; std::string thread; uint32_t pc = 0; uint32_t code[6] = {}; };
std::mutex g_hot_lock;
std::unordered_map<uint64_t, HotSample> g_hot;
const bool g_hot_on = getenv("KZ2_SPU_HOTSPOTS") != nullptr;

void hot_sample(const SpuThread &t) {
    uint32_t base = t.pc & (LS_SIZE - 256), h = 2166136261u;
    for (uint32_t k = 0; k < 256; k += 4) h = (h ^ t.ls32(base + k)) * 16777619u;
    uint64_t key = ((uint64_t)h << 32) | t.pc;
    std::lock_guard<std::mutex> lk(g_hot_lock);
    HotSample &s = g_hot[key];
    if (!s.count++) {
        s.thread = t.name, s.pc = t.pc;
        for (int k = 0; k < 6; k++) s.code[k] = t.ls32(t.pc + 4 * k);
    }
}

void hot_dump() {
    std::vector<std::pair<uint64_t, HotSample>> v;
    uint64_t total = 0;
    {
        std::lock_guard<std::mutex> lk(g_hot_lock);
        for (auto &[k, s] : g_hot) { v.push_back({k, s}); total += s.count; }
        g_hot.clear();
    }
    if (!total) return;
    // "task 3 (ELF 0xc59980)" -> "task (ELF 0xc59980)": one program, whichever task slot runs it
    auto program = [](std::string n) {
        if (n.rfind("task ", 0) == 0) { size_t p = n.find(' ', 5); if (p != std::string::npos) n = "task" + n.substr(p); }
        return n;
    };
    std::map<std::string, uint64_t> by_prog;
    struct Block { uint64_t count = 0; std::string prog; uint32_t base = 0, hash = 0, top_pc = 0; uint64_t top = 0; uint32_t code[3]; };
    std::map<std::pair<std::string, uint64_t>, Block> by_block;
    for (auto &[k, s] : v) {
        std::string p = program(s.thread);
        by_prog[p] += s.count;
        Block &b = by_block[{p, (k >> 32) << 32 | (s.pc & (LS_SIZE - 256))}];
        b.count += s.count, b.prog = p, b.base = s.pc & (LS_SIZE - 256), b.hash = (uint32_t)(k >> 32);
        if (s.count > b.top) b.top = s.count, b.top_pc = s.pc, memcpy(b.code, s.code, sizeof b.code);
    }
    std::vector<std::pair<std::string, uint64_t>> pv(by_prog.begin(), by_prog.end());
    std::sort(pv.begin(), pv.end(), [](auto &a, auto &b) { return a.second > b.second; });
    LOG("spu", "SPU time by program (%llu samples, 1 per 1024 insns):", (unsigned long long)total);
    for (size_t i = 0; i < pv.size() && i < 20; i++) LOG("spu", "  %5.1f%%  %s", 100.0 * pv[i].second / total, pv[i].first.c_str());
    std::vector<Block> bv;
    for (auto &[k, b] : by_block) bv.push_back(b);
    std::sort(bv.begin(), bv.end(), [](auto &a, auto &b) { return a.count > b.count; });
    LOG("spu", "hottest 256-byte code blocks:");
    for (size_t i = 0; i < bv.size() && i < 25; i++) {
        auto &b = bv[i];
        LOG("spu", "  %5.1f%%  %-26s %05x (code %08x) top pc %05x: %s | %s | %s", 100.0 * b.count / total, b.prog.c_str(),
            b.base, b.hash, b.top_pc, disasm(b.top_pc, b.code[0]).c_str(), disasm(b.top_pc + 4, b.code[1]).c_str(),
            disasm(b.top_pc + 8, b.code[2]).c_str());
    }
}
} // namespace

std::string stats_line(double secs, uint32_t frames) {
    static int calls;
    if (g_hot_on && ++calls % 6 == 0) hot_dump();
    static uint64_t last_i, last_r, last_w;
    uint64_t i = g_insns.load(), r = g_run_ns.load(), w = g_wait_ns.load();
    uint64_t di = i - last_i, dr = r - last_r, dw = w - last_w;
    last_i = i, last_r = r, last_w = w;
    char b[200];
    // busy = time in run() not blocked on events, summed over SPU threads (in cores)
    double busy = (double)(dr - std::min(dr, dw)) / 1e9;
    snprintf(b, sizeof b, "SPU %.1fM insns/frame, %.0fM insns/s, %.2f cores busy (%.0fM insns/s per busy core)",
             frames ? di / 1e6 / frames : 0.0, di / 1e6 / secs, busy / secs, busy > 0 ? di / 1e6 / busy : 0.0);
    return b;
}

void SpuThread::run() {
    running = true;
    // KZ2_SPU_TRACE=N logs the first N instructions executed by each SPU thread.
    static const int64_t trace_limit = getenv("KZ2_SPU_TRACE") ? atoll(getenv("KZ2_SPU_TRACE")) : 0;
    uint32_t event_poll = 0;
    uint64_t executed = 0;
    auto started = std::chrono::steady_clock::now();
    struct Flush {   // account on every exit path
        uint64_t &n;
        std::chrono::steady_clock::time_point t0;
        ~Flush() {
            g_insns += n;
            g_run_ns += std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count();
        }
    } flush{executed, started};
    while (running) {
        if (!(++executed & 1023) && g_hot_on) hot_sample(*this);
        // External interrupt: an enabled event is pending while interrupts are on. Checked
        // every 64 instructions: the check (lock-line compare, clock read) costs far more than
        // an instruction, and hardware delivers events asynchronously anyway.
        if (interrupts && !(++event_poll & 63) && pending_events()) {
            srr0 = pc;
            pc = 0;
            interrupts = false;
        }
        if (traps[pc >> 2] && on_trap) {
            if (!on_trap(*this)) { running = false; break; }
            continue;
        }
        uint32_t x = ls32(pc);
        history[history_pos++ & 63] = pc;
        if (traced < trace_limit && traced++ >= 0) LOG("spu", "%s %05x: %08x  %s", name.c_str(), pc, x, disasm(pc, x).c_str());
        uint32_t next = (pc + 4) & (LS_SIZE - 4);
        Op op = decode(x);
        uint32_t rt = x & 0x7f, ra = (x >> 7) & 0x7f, rb = (x >> 14) & 0x7f;
        const V &va = gpr[ra], &vb = gpr[rb];
        V r{};
        int32_t i7 = sext(rb, 7), i10 = sext((x >> 14) & 0x3ff, 10);
        uint32_t i16 = (x >> 7) & 0xffff;
        int32_t s16 = sext(i16, 16);
        // Indirect branches carry interrupt enable (E, bit 18) / disable (D, bit 19) flags.
        if (op == BI || op == BISL || op == BIZ || op == BINZ || op == BIHZ || op == BIHNZ || op == IRET || op == BISLED) {
            if (x & (1u << 18)) interrupts = true;
            if (x & (1u << 19)) interrupts = false;
        }
        switch (op) {
        /* ---------- memory ---------- */
        case LQD: gpr[rt] = lq((va.w[0] + (i10 << 4)) & (LS_SIZE - 16)); break;
        case LQX: gpr[rt] = lq((va.w[0] + vb.w[0]) & (LS_SIZE - 16)); break;
        case LQA: gpr[rt] = lq((i16 << 2) & (LS_SIZE - 16)); break;
        case LQR: gpr[rt] = lq((pc + (s16 << 2)) & (LS_SIZE - 16)); break;
        case STQD: stq((va.w[0] + (i10 << 4)) & (LS_SIZE - 16), gpr[rt]); break;
        case STQX: stq((va.w[0] + vb.w[0]) & (LS_SIZE - 16), gpr[rt]); break;
        case STQA: stq((i16 << 2) & (LS_SIZE - 16), gpr[rt]); break;
        case STQR: stq((pc + (s16 << 2)) & (LS_SIZE - 16), gpr[rt]); break;
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
            uint32_t b = (op == HEQ || op == HGT || op == HLGT) ? vb.w[0] : (uint32_t)i10;
            bool halt = op == HEQ || op == HEQI ? va.w[0] == b
                      : op == HGT || op == HGTI ? (int32_t)va.w[0] > (int32_t)b
                      : va.w[0] > b;
            if (halt) {
                LOG("spu", "%s: halt instruction at 0x%x", name.c_str(), pc);
                dump_history();
                running = false;
            }
            break;
        }

        /* ---------- branches ---------- */
        case BR: next = (pc + (s16 << 2)) & (LS_SIZE - 4); break;
        case BRA: next = (s16 << 2) & (LS_SIZE - 4); break;
        case BRSL: gpr[rt] = V{{next, 0, 0, 0}}; next = (pc + (s16 << 2)) & (LS_SIZE - 4); break;
        case BRASL: gpr[rt] = V{{next, 0, 0, 0}}; next = (s16 << 2) & (LS_SIZE - 4); break;
        case BI: next = va.w[0] & (LS_SIZE - 4); break;
        case IRET: next = srr0 & (LS_SIZE - 4); break;
        case BISLED: case BISL: { uint32_t t = va.w[0] & (LS_SIZE - 4); gpr[rt] = V{{next, 0, 0, 0}}; if (op == BISL) next = t; break; }
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
        case FA: for (int i = 0; i < 4; i++) r.set_f(i, ftz(ftz(va.f(i)) + ftz(vb.f(i)))); gpr[rt] = r; break;
        case FS: for (int i = 0; i < 4; i++) r.set_f(i, ftz(ftz(va.f(i)) - ftz(vb.f(i)))); gpr[rt] = r; break;
        case FM: for (int i = 0; i < 4; i++) r.set_f(i, ftz(ftz(va.f(i)) * ftz(vb.f(i)))); gpr[rt] = r; break;
        case FMA: { const V &C = gpr[rt]; for (int i = 0; i < 4; i++) r.set_f(i, ftz(ftz(va.f(i)) * ftz(vb.f(i)) + ftz(C.f(i)))); gpr[(x >> 21) & 0x7f] = r; break; }
        case FMS: { const V &C = gpr[rt]; for (int i = 0; i < 4; i++) r.set_f(i, ftz(ftz(va.f(i)) * ftz(vb.f(i)) - ftz(C.f(i)))); gpr[(x >> 21) & 0x7f] = r; break; }
        case FNMS: { const V &C = gpr[rt]; for (int i = 0; i < 4; i++) r.set_f(i, ftz(ftz(C.f(i)) - ftz(va.f(i)) * ftz(vb.f(i)))); gpr[(x >> 21) & 0x7f] = r; break; }
        // frest/frsqest return the exact value; fi then passes it through.
        case FREST: for (int i = 0; i < 4; i++) r.set_f(i, ftz(1.0f / ftz(va.f(i)))); gpr[rt] = r; break;
        case FRSQEST: for (int i = 0; i < 4; i++) r.set_f(i, ftz(1.0f / std::sqrt(std::fabs(ftz(va.f(i)))))); gpr[rt] = r; break;
        case FI: gpr[rt] = vb; break;
        case FCEQ: for (int i = 0; i < 4; i++) r.w[i] = ftz(va.f(i)) == ftz(vb.f(i)) ? ~0u : 0; gpr[rt] = r; break;
        case FCMEQ: for (int i = 0; i < 4; i++) r.w[i] = std::fabs(ftz(va.f(i))) == std::fabs(ftz(vb.f(i))) ? ~0u : 0; gpr[rt] = r; break;
        case FCGT: for (int i = 0; i < 4; i++) r.w[i] = ftz(va.f(i)) > ftz(vb.f(i)) ? ~0u : 0; gpr[rt] = r; break;
        case FCMGT: for (int i = 0; i < 4; i++) r.w[i] = std::fabs(ftz(va.f(i))) > std::fabs(ftz(vb.f(i))) ? ~0u : 0; gpr[rt] = r; break;
        case CSFLT: { int sc = 155 - (int)((x >> 14) & 0xff); for (int i = 0; i < 4; i++) r.set_f(i, (float)std::ldexp((double)(int32_t)va.w[i], -sc)); gpr[rt] = r; break; }
        case CUFLT: { int sc = 155 - (int)((x >> 14) & 0xff); for (int i = 0; i < 4; i++) r.set_f(i, (float)std::ldexp((double)va.w[i], -sc)); gpr[rt] = r; break; }
        case CFLTS: { int sc = 173 - (int)((x >> 14) & 0xff); for (int i = 0; i < 4; i++) r.w[i] = (uint32_t)sat_s32(std::ldexp((double)va.f(i), sc)); gpr[rt] = r; break; }
        case CFLTU: { int sc = 173 - (int)((x >> 14) & 0xff); for (int i = 0; i < 4; i++) r.w[i] = sat_u32(std::ldexp((double)va.f(i), sc)); gpr[rt] = r; break; }
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
        case RDCH: gpr[rt] = V{{read_channel(ra), 0, 0, 0}}; break;
        case RCHCNT: gpr[rt] = V{{channel_count(ra), 0, 0, 0}}; break;
        case WRCH: write_channel(ra, gpr[rt].w[0]); break;
        case MFSPR: gpr[rt] = V{}; break;
        case MTSPR: break;
        case STOP: case STOPD:
            pc = next;
            if (!on_stop || !on_stop(*this, x & 0x3fff)) running = false;
            continue;

        case INVALID: default:
            LOG("spu", "%s: unknown instruction %08x at 0x%x", name.c_str(), x, pc);
            dump_history();
            running = false;
            continue;
        }
        pc = next;
    }
}

} // namespace spu
