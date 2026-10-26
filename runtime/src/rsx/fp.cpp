// RSX fragment program microcode -> HLSL pixel shader.
//
// Programs live in memory as 16-byte instructions whose 32-bit words have
// their 16-bit halves swapped. A source of type "constant" means the next
// 16 bytes hold that constant inline.
#include "rsx.h"

#include <cstdio>
#include <set>

namespace rsx {
namespace {
inline uint32_t bits(uint32_t v, int lo, int n) { return (v >> lo) & ((1u << n) - 1); }
inline uint32_t fpword(uint32_t ea) { uint32_t w = ld32(ea); return (w << 16) | (w >> 16); }
const char *SW = "xyzw";

// fragment inputs (src_attr_reg_num) -> vertex shader output register
std::string input_reg(uint32_t n) {
    switch (n) {
    case 0: return "wpos";
    case 1: return "i.o1";              // COL0
    case 2: return "i.o2";              // COL1
    case 3: return "i.o5.xxxx";         // FOGC
    case 14: return "float4(face, face, face, face)";
    default:
        if (n >= 4 && n <= 11) return "i.o" + std::to_string(7 + (n - 4));   // TEX0..7
        if (n == 12) return "i.o15";                                          // TEX8
        if (n == 13) return "i.o6";                                           // TEX9
        return "float4(0,0,0,0)";
    }
}

struct Ctx {
    uint32_t dst, s[3];
    std::string cst;    // inline constant, if any
};

std::string src_expr(const Ctx &c, int n) {
    uint32_t s = c.s[n];
    uint32_t type = bits(s, 0, 2);
    std::string r;
    if (type == 0) r = (bits(s, 8, 1) ? "h[" : "r[") + std::to_string(bits(s, 2, 6)) + "]";
    else if (type == 1) r = input_reg(bits(c.dst, 13, 4));
    else if (type == 2) r = c.cst;
    else r = "float4(0,0,0,0)";
    std::string sw = ".";
    sw += SW[bits(s, 9, 2)]; sw += SW[bits(s, 11, 2)]; sw += SW[bits(s, 13, 2)]; sw += SW[bits(s, 15, 2)];
    if (sw != ".xyzw") r = "(" + r + ")" + sw;
    // abs lives in a different bit for src0 than for src1/src2
    bool abs_ = n == 0 ? bits(s, 29, 1) : bits(s, 18, 1);
    if (abs_) r = "abs(" + r + ")";
    if (bits(s, 17, 1)) r = "(-" + r + ")";
    return r;
}

std::string cmp_cc(bool lt, bool eq, bool gt, const std::string &v) {
    if (lt && eq && gt) return "true";
    if (!lt && !eq && !gt) return "false";
    if (lt && eq) return v + " <= 0";
    if (gt && eq) return v + " >= 0";
    if (lt && gt) return v + " != 0";
    if (lt) return v + " < 0";
    if (gt) return v + " > 0";
    return v + " == 0";
}
} // namespace

FpScan scan_fp(uint32_t addr) {
    // Instruction words only (inline constants excluded) identify the shader;
    // constant values are uploaded per draw.
    FpScan r{1469598103934665603ull, {}};
    uint32_t a = addr;
    for (int n = 0; n < 1024; n++) {
        uint32_t w[4] = {fpword(a), fpword(a + 4), fpword(a + 8), fpword(a + 12)};
        r.hash = fnv(w, 16, r.hash);
        a += 16;
        if (bits(w[1], 0, 2) == 2 || bits(w[2], 0, 2) == 2 || bits(w[3], 0, 2) == 2) {
            if (r.consts.size() < 128) r.consts.push_back(a - addr);
            a += 16;
        }
        if (bits(w[0], 0, 1)) break;
    }
    return r;
}

uint32_t fp_const_word(uint32_t ea) { return fpword(ea); }

FpInfo translate_fp(const FpKey &key) {
    std::string body;
    std::set<std::string> warn;
    bool used_tex[16] = {};
    int nconst = 0;
    uint64_t wrote_r = 0, wrote_h = 0;  // which registers the program writes
    uint32_t a = key.addr;
    for (int n = 0; n < 1024; n++) {
        Ctx c{fpword(a), {fpword(a + 4), fpword(a + 8), fpword(a + 12)}, ""};
        a += 16;
        bool has_const = bits(c.s[0], 0, 2) == 2 || bits(c.s[1], 0, 2) == 2 || bits(c.s[2], 0, 2) == 2;
        if (has_const) {
            c.cst = "fc[" + std::to_string(nconst++) + "]";
            a += 16;
        }
        uint32_t op = bits(c.dst, 24, 6) | (bits(c.s[1], 31, 1) << 6);
        std::string s0 = src_expr(c, 0), s1 = src_expr(c, 1), s2 = src_expr(c, 2);
        uint32_t tex = bits(c.dst, 17, 4);
        std::string texs = "t" + std::to_string(tex), smp = "s" + std::to_string(tex);
        auto sample = [&](const std::string &coord, const std::string &fn = "Sample", const std::string &extra = "") {
            used_tex[tex] = true;
            uint8_t dim = key.texdim[tex] & TEX_DIM_MASK;
            std::string cs = dim == TEX_2D ? "(" + coord + ").xy" : "(" + coord + ").xyz";
            if (key.texdim[tex] & TEX_UNNORM) cs = "(" + cs + " * texscale[" + std::to_string(tex) + "].xy)";
            // Shadow lookup: compare z against the stored depth, filtered (PCF); every component is the result.
            if ((key.texdim[tex] & TEX_SHADOW) && dim == TEX_2D)
                return texs + ".SampleCmpLevelZero(" + smp + ", " + cs + ", (" + coord + ").z).xxxx";
            std::string e = texs + "." + fn + "(" + smp + ", " + cs + extra + ")";
            // TEXTURE_CONTROL1 remap: per output channel a source (A, R, G, B) or the constant 0 / 1.
            uint16_t rm = key.remap[tex];
            if (rm && rm != 0xaae4)
                e = "remap(" + e + ", uint4(" + std::to_string(rm & 0xff) + ", " + std::to_string(rm >> 8) + ", 0, 0))";
            return e;
        };
        std::string e;
        bool scalar = false;
        switch (op) {
        case 0x00: break;
        case 0x01: e = s0; break;
        case 0x02: e = s0 + " * " + s1; break;
        case 0x03: e = s0 + " + " + s1; break;
        case 0x04: e = s0 + " * " + s1 + " + " + s2; break;
        case 0x05: e = "dot((" + s0 + ").xyz, (" + s1 + ").xyz).xxxx"; break;
        case 0x06: e = "dot(" + s0 + ", " + s1 + ").xxxx"; break;
        case 0x07: e = "float4(1, (" + s0 + ").y * (" + s1 + ").y, (" + s0 + ").z, (" + s1 + ").w)"; break;
        case 0x08: e = "min(" + s0 + ", " + s1 + ")"; break;
        case 0x09: e = "max(" + s0 + ", " + s1 + ")"; break;
        case 0x0a: e = "float4(" + s0 + " < " + s1 + ")"; break;
        case 0x0b: e = "float4(" + s0 + " >= " + s1 + ")"; break;
        case 0x0c: e = "float4(" + s0 + " <= " + s1 + ")"; break;
        case 0x0d: e = "float4(" + s0 + " > " + s1 + ")"; break;
        case 0x0e: e = "float4(" + s0 + " != " + s1 + ")"; break;
        case 0x0f: e = "float4(" + s0 + " == " + s1 + ")"; break;
        case 0x10: e = "frac(" + s0 + ")"; break;
        case 0x11: e = "floor(" + s0 + ")"; break;
        case 0x12: e = "KIL"; break;
        case 0x13: e = "pk4(" + s0 + ")"; break;
        case 0x14: e = "up4(" + s0 + ")"; break;
        case 0x15: e = "ddx(" + s0 + ")"; break;
        case 0x16: e = "ddy(" + s0 + ")"; break;
        case 0x17: e = sample(s0); break;
        case 0x18: e = sample("(" + s0 + ") / (" + s0 + ").w"); break;
        case 0x19: e = sample(s0, "SampleGrad", ", (" + s1 + ").xy, (" + s2 + ").xy"); break;
        case 0x1a: e = "(1.0 / (" + s0 + ").xxxx)"; scalar = true; break;
        case 0x1b: e = "rsqrt(abs((" + s0 + ").xxxx))"; break;
        case 0x1c: e = "exp2((" + s0 + ").xxxx)"; break;
        case 0x1d: e = "log2((" + s0 + ").xxxx)"; break;
        case 0x1e: e = "lit((" + s0 + ").x, (" + s0 + ").y, (" + s0 + ").w)"; break;
        case 0x1f: e = "lerp(" + s2 + ", " + s1 + ", " + s0 + ")"; break;
        case 0x20: e = "float4(1,1,1,1)"; break;
        case 0x21: e = "float4(0,0,0,0)"; break;
        case 0x22: e = "cos((" + s0 + ").xxxx)"; break;
        case 0x23: e = "sin((" + s0 + ").xxxx)"; break;
        case 0x24: e = "pk2(" + s0 + ")"; break;
        case 0x25: e = "up2(" + s0 + ")"; break;
        case 0x26: e = "pow(abs((" + s0 + ").xxxx), (" + s1 + ").xxxx)"; break;
        case 0x27: e = "pk4(" + s0 + ")"; break;   // PKB ~ unsigned bytes
        case 0x28: e = "up4(" + s0 + ")"; break;
        case 0x29: e = "pk2(" + s0 + ")"; break;   // PK16
        case 0x2a: e = "up2(" + s0 + ")"; break;
        case 0x2c: e = "pk4(" + s0 + ")"; break;   // PKG
        case 0x2d: e = "up4(" + s0 + ")"; break;
        case 0x2e: e = "(dot((" + s0 + ").xy, (" + s1 + ").xy) + (" + s2 + ").x).xxxx"; break;
        case 0x2f: e = sample(s0, "SampleLevel", ", (" + s1 + ").x"); break;
        case 0x31: e = sample(s0, "SampleBias", ", (" + s1 + ").x"); break;
        case 0x36: e = "(" + s1 + " - 2 * dot((" + s0 + ").xyz, (" + s1 + ").xyz) * " + s0 + ")"; break;
        case 0x38: e = "dot((" + s0 + ").xy, (" + s1 + ").xy).xxxx"; break;
        case 0x39: e = "float4(normalize((" + s0 + ").xyz), 0)"; break;
        case 0x3a: e = "(" + s0 + " / (" + s1 + ").xxxx)"; break;
        case 0x3b: e = "(" + s0 + " * rsqrt(abs((" + s1 + ").xxxx)))"; break;
        case 0x3c: e = "lit((" + s0 + ").x, (" + s0 + ").y, (" + s0 + ").w)"; break;
        case 0x3d: case 0x3e: break;  // fences
        default: warn.insert("fp op 0x" + [&] { char b[8]; snprintf(b, 8, "%x", op); return std::string(b); }()); break;
        }
        (void)scalar;
        if (bits(c.dst, 21, 1) && !e.empty() && e != "KIL" && op >= 0x17 && op <= 0x19) e = "(" + e + " * 2 - 1)"; // _bx2
        // result scale and saturation
        switch (bits(c.s[1], 28, 3)) {
        case 1: e = "(" + e + ") * 2"; break;
        case 2: e = "(" + e + ") * 4"; break;
        case 3: e = "(" + e + ") * 8"; break;
        case 5: e = "(" + e + ") / 2"; break;
        case 6: e = "(" + e + ") / 4"; break;
        case 7: e = "(" + e + ") / 8"; break;
        }
        if (bits(c.dst, 31, 1) && e != "KIL" && !e.empty()) e = "saturate(" + e + ")";

        // condition for executing this instruction (per component)
        bool lt = bits(c.s[0], 18, 1), eq = bits(c.s[0], 19, 1), gt = bits(c.s[0], 20, 1);
        std::string cc = bits(c.s[0], 31, 1) ? "cc1" : "cc0";
        uint32_t csw[4] = {bits(c.s[0], 21, 2), bits(c.s[0], 23, 2), bits(c.s[0], 25, 2), bits(c.s[0], 27, 2)};
        auto cond = [&](int comp) { return cmp_cc(lt, eq, gt, cc + "." + SW[csw[comp]]); };

        if (e == "KIL") {
            body += "  if (" + cond(0) + ") discard;\n";
        } else if (!e.empty()) {
            bool m[4] = {(bool)bits(c.dst, 9, 1), (bool)bits(c.dst, 10, 1), (bool)bits(c.dst, 11, 1), (bool)bits(c.dst, 12, 1)};
            std::string dreg = (bits(c.dst, 7, 1) ? "h[" : "r[") + std::to_string(bits(c.dst, 1, 6)) + "]";
            if (!bits(c.dst, 30, 1)) (bits(c.dst, 7, 1) ? wrote_h : wrote_r) |= 1ull << bits(c.dst, 1, 6);
            bool nodest = bits(c.dst, 30, 1);
            std::string ccw = bits(c.s[0], 30, 1) ? "cc1" : "cc0";
            body += "  { float4 t = " + e + ";\n";
            for (int k = 0; k < 4; k++) {
                if (!m[k]) continue;
                std::string w;
                if (!nodest) w += dreg + "." + SW[k] + " = t." + SW[k] + "; ";
                if (bits(c.dst, 8, 1)) w += ccw + "." + SW[k] + " = t." + SW[k] + "; ";
                if (w.empty()) continue;
                std::string cnd = cond(k);
                body += cnd == "true" ? "    " + w + "\n" : "    if (" + cnd + ") { " + w + "}\n";
            }
            body += "  }\n";
        }
        if (bits(c.dst, 0, 1)) break;
    }
    std::string w;
    for (auto &s : warn) w += "// warning: " + s + "\n";
    if (!warn.empty()) {
        static int logged;
        if (logged++ < 20) LOG("rsx", "fragment program 0x%x: unsupported features:%s", key.addr, w.c_str());
    }

    // R and H registers alias on hardware (R0 = H0:H1). Export from whichever
    // view the program actually wrote; the control bit decides only when both are.
    bool fp32 = (wrote_r & 1) && !(wrote_h & 1) ? true : (wrote_h & 1) && !(wrote_r & 1) ? false : (key.control & 0x40) != 0;
    std::string h = w;
    for (int t = 0; t < 16; t++) {
        if (!used_tex[t]) continue;
        uint8_t dim = key.texdim[t] & TEX_DIM_MASK;
        const char *ty = dim == TEX_CUBE ? "TextureCube" : dim == TEX_3D ? "Texture3D" : "Texture2D";
        h += std::string(ty) + " t" + std::to_string(t) + " : register(t" + std::to_string(t) + ");\n";
        h += std::string(key.texdim[t] & TEX_SHADOW ? "SamplerComparisonState" : "SamplerState") + " s" +
             std::to_string(t) + " : register(s" + std::to_string(t) + ");\n";
    }
    h += "cbuffer PX : register(b0) { float4 alpha_ref; float4 wpos_info; float4 texscale[16]; float4 fc[128]; };\n"
         "float4 pk4(float4 v) { uint4 b = (uint4)round(saturate(v) * 255); return asfloat((b.x | b.y << 8 | b.z << 16 | b.w << 24)).xxxx; }\n"
         "float4 up4(float4 v) { uint u = asuint(v.x); return float4(u & 255, (u >> 8) & 255, (u >> 16) & 255, u >> 24) / 255.0; }\n"
         "float4 pk2(float4 v) { return asfloat(f32tof16(v.x) | (f32tof16(v.y) << 16)).xxxx; }\n"
         "float4 up2(float4 v) { uint u = asuint(v.x); return float4(f16tof32(u & 0xffff), f16tof32(u >> 16), 0, 0); }\n"
         // m.x: 2-bit sources (0 A, 1 R, 2 G, 3 B) for outputs A, R, G, B; m.y: their types (0 zero, 1 one, 2 source)
         "float rmc(float4 t, uint2 m, uint i) { uint s = (m.x >> (2 * i)) & 3, k = (m.y >> (2 * i)) & 3;\n"
         "  return k == 2 ? (s == 0 ? t.a : s == 1 ? t.r : s == 2 ? t.g : t.b) : k == 1 ? 1 : 0; }\n"
         "float4 remap(float4 t, uint4 m) { return float4(rmc(t, m.xy, 1), rmc(t, m.xy, 2), rmc(t, m.xy, 3), rmc(t, m.xy, 0)); }\n"
         "struct PIn { float4 pos : SV_Position;\n";
    for (int k = 1; k < 16; k++) h += "  float4 o" + std::to_string(k) + " : TEXCOORD" + std::to_string(k) + ";\n";
    h += "  bool front : SV_IsFrontFace; };\n"
         "struct POut { float4 c0 : SV_Target0; float4 c1 : SV_Target1; float4 c2 : SV_Target2; float4 c3 : SV_Target3; };\n"
         "POut main(PIn i) {\n"
         "  float4 r[64]; [unroll] for (int k = 0; k < 64; k++) r[k] = 0;\n"
         "  float4 h[64]; [unroll] for (int k = 0; k < 64; k++) h[k] = 0;\n"
         "  float4 cc0 = 0, cc1 = 0;\n"
         "  float face = i.front ? 1 : -1;\n"
         // wpos_info: x = origin bottom, y = window height, zw = multisample scale (pixel = sample / scale)
         "  float2 px = floor(i.pos.xy / wpos_info.zw) + 0.5;\n"
         "  float4 wpos = float4(px.x, wpos_info.x > 0 ? wpos_info.y - px.y : px.y, i.pos.z, 1 / i.pos.w);\n" +
         body + "  POut o;\n";
    if (fp32) h += "  o.c0 = r[0]; o.c1 = r[2]; o.c2 = r[3]; o.c3 = r[4];\n";
    else h += "  o.c0 = h[0]; o.c1 = h[4]; o.c2 = h[6]; o.c3 = h[8];\n";
    if (key.alpha_func) {
        static const char *ops[] = {"false", "<", "==", "<=", ">", "!=", ">=", "true"};
        uint32_t f = key.alpha_func - 0x200;
        if (f < 8 && f != 7) {
            h += f == 0 ? "  discard;\n" : std::string("  if (!(o.c0.a ") + ops[f] + " alpha_ref.x)) discard;\n";
        }
    }
    h += "  return o;\n}\n";
    return {h, fnv(h.data(), h.size()), 4};
}
} // namespace rsx
