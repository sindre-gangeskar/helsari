// RSX vertex program microcode -> HLSL vertex shader.
//
// Each instruction is 4 words (D0..D3) issuing one vector op and one scalar
// op in parallel. Source operands are 17-bit fields split across words.
#include "rsx.h"

#include <cstdio>
#include <set>

namespace rsx {
namespace {
inline uint32_t bits(uint32_t v, int lo, int n) { return (v >> lo) & ((1u << n) - 1); }

const char *SW = "xyzw";

struct Insn {
    uint32_t d0, d1, d2, d3;
    uint32_t src(int n) const {
        if (n == 0) return bits(d2, 23, 9) | (bits(d1, 0, 8) << 9);
        if (n == 1) return bits(d2, 6, 17);
        return bits(d3, 21, 11) | (bits(d2, 0, 6) << 11);
    }
};

std::string src_expr(const Insn &in, int n, std::set<std::string> *warn) {
    uint32_t s = in.src(n);
    uint32_t type = bits(s, 0, 2);
    std::string r;
    switch (type) {
    case 1: r = "r[" + std::to_string(bits(s, 2, 6)) + "]"; break;
    case 2: r = "v[" + std::to_string(bits(in.d1, 8, 4)) + "]"; break;
    case 3: {
        uint32_t ci = bits(in.d1, 12, 10);
        if (bits(in.d3, 1, 1)) {  // relative addressing through an address register
            std::string a = std::string("a") + (bits(in.d0, 24, 1) ? "1" : "0") + "." + SW[bits(in.d0, 0, 2)];
            r = "c[(" + std::to_string(ci) + " + " + a + ") & 511]";
        } else {
            r = "c[" + std::to_string(ci) + "]";
        }
        break;
    }
    default: r = "float4(0,0,0,0)"; if (warn) warn->insert("bad src type"); break;
    }
    std::string sw = ".";
    sw += SW[bits(s, 14, 2)]; sw += SW[bits(s, 12, 2)]; sw += SW[bits(s, 10, 2)]; sw += SW[bits(s, 8, 2)];
    if (sw != ".xyzw") r += sw;
    if (bits(in.d0, 21 + n, 1)) r = "abs(" + r + ")";
    if (bits(s, 16, 1)) r = "(-" + r + ")";
    return r;
}

// Condition test against a condition register component; RSX cond codes:
// 0 FL, 1 LT, 2 EQ, 3 LE, 4 GT, 5 NE, 6 GE, 7 TR
std::string cond_test(uint32_t cond, const std::string &v) {
    switch (cond) {
    case 0: return "false";
    case 1: return v + " < 0";
    case 2: return v + " == 0";
    case 3: return v + " <= 0";
    case 4: return v + " > 0";
    case 5: return v + " != 0";
    case 6: return v + " >= 0";
    default: return "true";
    }
}

// Emit `dst.mask = value` honouring the optional per-component condition.
void emit_write(std::string &o, const Insn &in, const std::string &dst, const std::string &val, const bool mask[4]) {
    bool test = bits(in.d0, 13, 1);
    uint32_t cond = bits(in.d0, 10, 3);
    std::string cc = bits(in.d0, 25, 1) ? "cc1" : "cc0";
    uint32_t csw[4] = {bits(in.d0, 8, 2), bits(in.d0, 6, 2), bits(in.d0, 4, 2), bits(in.d0, 2, 2)};
    for (int c = 0; c < 4; c++) {
        if (!mask[c]) continue;
        std::string line = dst + "." + SW[c] + " = " + val + "." + SW[c] + ";";
        if (test && cond != 7) line = "if (" + cond_test(cond, cc + "." + SW[csw[c]]) + ") " + line;
        o += "  " + line + "\n";
    }
}
} // namespace

VpInfo translate_vp(uint32_t start) {
    std::string body;
    std::set<std::string> warn;
    int n = 0;
    for (uint32_t i = start; i < 512; i++, n++) {
        Insn in{vp_ucode[i * 4], vp_ucode[i * 4 + 1], vp_ucode[i * 4 + 2], vp_ucode[i * 4 + 3]};
        uint32_t vop = bits(in.d1, 22, 5), sop = bits(in.d1, 27, 5);
        std::string s0 = src_expr(in, 0, &warn), s1 = src_expr(in, 1, &warn), s2 = src_expr(in, 2, &warn);
        std::string sc = "(" + s2 + ").xxxx";  // the scalar unit reads src2
        bool sat = bits(in.d0, 26, 1);
        std::string vexpr, sexpr;
        switch (vop) {
        case 0: break;
        case 1: vexpr = s0; break;
        case 2: vexpr = s0 + " * " + s1; break;
        case 3: vexpr = s0 + " + " + s2; break;
        case 4: vexpr = s0 + " * " + s1 + " + " + s2; break;
        case 5: vexpr = "dot((" + s0 + ").xyz, (" + s1 + ").xyz).xxxx"; break;
        case 6: vexpr = "dot(float4((" + s0 + ").xyz, 1), " + s1 + ").xxxx"; break;
        case 7: vexpr = "dot(" + s0 + ", " + s1 + ").xxxx"; break;
        case 8: vexpr = "float4(1, (" + s0 + ").y * (" + s1 + ").y, (" + s0 + ").z, (" + s1 + ").w)"; break;
        case 9: vexpr = "min(" + s0 + ", " + s1 + ")"; break;
        case 10: vexpr = "max(" + s0 + ", " + s1 + ")"; break;
        case 11: vexpr = "float4(" + s0 + " < " + s1 + ")"; break;
        case 12: vexpr = "float4(" + s0 + " >= " + s1 + ")"; break;
        case 13: break;  // ARL, handled below
        case 14: vexpr = "frac(" + s0 + ")"; break;
        case 15: vexpr = "floor(" + s0 + ")"; break;
        case 16: vexpr = "float4(" + s0 + " == " + s1 + ")"; break;
        case 17: vexpr = "float4(0,0,0,0)"; break;
        case 18: vexpr = "float4(" + s0 + " > " + s1 + ")"; break;
        case 19: vexpr = "float4(" + s0 + " <= " + s1 + ")"; break;
        case 20: vexpr = "float4(" + s0 + " != " + s1 + ")"; break;
        case 21: vexpr = "float4(1,1,1,1)"; break;
        case 22: vexpr = "sign(" + s0 + ")"; break;
        default: warn.insert("vec op " + std::to_string(vop)); break;
        }
        switch (sop) {
        case 0: break;
        case 1: sexpr = sc; break;
        case 2: sexpr = "(1.0 / " + sc + ")"; break;
        case 3: sexpr = "clamp(1.0 / " + sc + ", 5.42101e-20, 1.884467e19)"; break;
        case 4: sexpr = "rsqrt(abs(" + sc + "))"; break;
        case 5: sexpr = "float4(exp2(floor(" + sc + ".x)), frac(" + sc + ".x), exp2(" + sc + ".x), 1)"; break;
        case 6: sexpr = "float4(floor(log2(abs(" + sc + ".x))), abs(" + sc + ".x) / exp2(floor(log2(abs(" + sc + ".x)))), log2(abs(" + sc + ".x)), 1)"; break;
        case 7: sexpr = "lit((" + s2 + ").x, (" + s2 + ").y, (" + s2 + ").w)"; break;
        case 13: sexpr = "log2(" + sc + ")"; break;
        case 14: sexpr = "exp2(" + sc + ")"; break;
        case 15: sexpr = "sin(" + sc + ")"; break;
        case 16: sexpr = "cos(" + sc + ")"; break;
        default: warn.insert("sca op " + std::to_string(sop)); break;
        }
        // Read both results before any write (dual issue).
        body += "  {\n";
        if (!vexpr.empty()) body += "  float4 vr = " + (sat ? "saturate(" + vexpr + ")" : vexpr) + ";\n";
        if (!sexpr.empty()) body += "  float4 sr = " + (sat ? "saturate(" + sexpr + ")" : sexpr) + ";\n";
        if (vop == 13) {
            body += "  a" + std::to_string(bits(in.d0, 15, 6) & 1) + " = int4(floor(" + s0 + "));\n";
        }
        uint32_t dst = bits(in.d3, 2, 5), dtmp = bits(in.d0, 15, 6), stmp = bits(in.d3, 7, 6);
        bool vm[4] = {(bool)bits(in.d3, 16, 1), (bool)bits(in.d3, 15, 1), (bool)bits(in.d3, 14, 1), (bool)bits(in.d3, 13, 1)};
        bool sm[4] = {(bool)bits(in.d3, 20, 1), (bool)bits(in.d3, 19, 1), (bool)bits(in.d3, 18, 1), (bool)bits(in.d3, 17, 1)};
        bool cc_update = bits(in.d0, 14, 1) && bits(in.d0, 29, 1);
        std::string ccreg = bits(in.d0, 25, 1) ? "cc1" : "cc0";
        if (!vexpr.empty()) {
            if (dtmp != 0x3f) emit_write(body, in, "r[" + std::to_string(dtmp) + "]", "vr", vm);
            if (dst != 0x1f) emit_write(body, in, "o[" + std::to_string(dst & 15) + "]", "vr", vm);
            if (cc_update) emit_write(body, in, ccreg, "vr", vm);
        }
        if (!sexpr.empty()) {
            if (stmp != 0x3f) emit_write(body, in, "r[" + std::to_string(stmp) + "]", "sr", sm);
            else if (dst != 0x1f) emit_write(body, in, "o[" + std::to_string(dst & 15) + "]", "sr", sm);
            if (cc_update && vexpr.empty()) emit_write(body, in, ccreg, "sr", sm);
        }
        body += "  }\n";
        if (bits(in.d3, 0, 1)) break;
    }
    std::string w;
    for (auto &s : warn) w += "// warning: " + s + "\n";
    if (!warn.empty()) {
        static int logged;
        if (logged++ < 20) LOG("rsx", "vertex program at %u: unsupported features:%s", start, w.c_str());
    }
    std::string h = w +
        "cbuffer VC : register(b0) { float4 c[512]; };\n"
        "cbuffer VX : register(b1) { float4 vscale; float4 voffset; float4 surf; };\n"
        "StructuredBuffer<float4> vtx : register(t0);\n"
        "struct VOut { float4 pos : SV_Position;\n";
    for (int k = 1; k < 16; k++) h += "  float4 o" + std::to_string(k) + " : TEXCOORD" + std::to_string(k) + ";\n";
    h += "};\n"
         "VOut main(uint vid : SV_VertexID) {\n"
         "  float4 v[16];\n"
         "  [unroll] for (int k = 0; k < 16; k++) v[k] = vtx[vid * 16 + k];\n"
         "  float4 r[32]; [unroll] for (int k = 0; k < 32; k++) r[k] = 0;\n"
         "  float4 o[16]; [unroll] for (int k = 0; k < 16; k++) o[k] = float4(0, 0, 0, 1);\n"
         "  float4 cc0 = 0, cc1 = 0; int4 a0 = 0, a1 = 0;\n" +
         body +
         "  VOut res;\n"
         // RSX viewport transform, then map window coords to D3D clip space.
         "  float4 p = o[0];\n"
         "  float3 win = (p.xyz / p.w) * vscale.xyz + voffset.xyz;\n"
         "  float2 ndc = float2(win.x / surf.x * 2 - 1, surf.z > 0 ? win.y / surf.y * 2 - 1 : 1 - win.y / surf.y * 2);\n"
         "  res.pos = float4(ndc * p.w, win.z * p.w, p.w);\n";
    for (int k = 1; k < 16; k++) h += "  res.o" + std::to_string(k) + " = o[" + std::to_string(k) + "];\n";
    h += "  return res;\n}\n";
    return {h, fnv(h.data(), h.size())};
}
} // namespace rsx
