// RSX -> Direct3D 11 backend. Runs entirely on the FIFO thread.
//
//  * Render targets are D3D textures keyed by their RSX address. Sampling a
//    texture at a render target's address uses the D3D copy directly.
//  * Vertex attributes are converted on the CPU into 16 float4 per vertex and
//    read in the vertex shader through SV_VertexID.
//  * Shaders are translated from RSX microcode and cached by microcode hash.
#include "rsx.h"

#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>

#include <cmath>
#include <filesystem>
#include <unordered_map>

namespace rsx {
uint32_t regs[0x10000 / 4];
uint32_t vp_ucode[512 * 4];
float vp_consts[512 * 4];
bool vp_consts_dirty = true;

uint64_t fnv(const void *p, size_t n, uint64_t h) {
    const uint8_t *b = (const uint8_t *)p;
    for (size_t i = 0; i < n; i++) h = (h ^ b[i]) * 1099511628211ull;
    return h;
}

namespace {
template <class T> void release(T *&p) { if (p) { p->Release(); p = nullptr; } }

HWND g_hwnd;
ID3D11Device *g_dev;
ID3D11DeviceContext *g_ctx;
IDXGISwapChain *g_swap;
ID3D11RenderTargetView *g_backbuffer;
bool g_ok;

/* ---------------- state tracked from methods ---------------- */
uint32_t g_vp_load, g_vc_load;
float g_vattr[16][4];             // constant attribute values (when an array is disabled)
uint32_t g_prim;
struct Range { uint32_t first, count; };
std::vector<Range> g_ranges;
bool g_indexed;
uint32_t g_frame;
int g_draws_this_frame;

/* ---------------- shaders ---------------- */
ID3D11VertexShader *g_present_vs;
ID3D11PixelShader *g_present_ps;
std::unordered_map<uint64_t, ID3D11VertexShader *> g_vs;
struct PsEntry { ID3D11PixelShader *ps; std::vector<uint32_t> consts; };
std::unordered_map<uint64_t, PsEntry> g_ps;

ID3DBlob *compile(const std::string &src, const char *target, uint64_t hash) {
    ID3DBlob *code = nullptr, *err = nullptr;
    HRESULT hr = D3DCompile(src.data(), src.size(), nullptr, nullptr, nullptr, "main", target,
                            D3DCOMPILE_OPTIMIZATION_LEVEL1, 0, &code, &err);
    if (FAILED(hr)) {
        std::filesystem::create_directories("shaders");
        char path[64];
        snprintf(path, sizeof path, "shaders/%s_%016llx.hlsl", target, (unsigned long long)hash);
        if (FILE *f = fopen(path, "w")) { fwrite(src.data(), 1, src.size(), f); fclose(f); }
        LOG("rsx", "shader compile failed (%s): %s", path, err ? (const char *)err->GetBufferPointer() : "?");
        release(err);
        return nullptr;
    }
    release(err);
    if (getenv("KZ2_DUMP_SHADERS")) {
        std::filesystem::create_directories("shaders");
        char path[64];
        snprintf(path, sizeof path, "shaders/%s_%016llx.hlsl", target, (unsigned long long)hash);
        if (FILE *f = fopen(path, "w")) { fwrite(src.data(), 1, src.size(), f); fclose(f); }
    }
    return code;
}

/* ---------------- dynamic buffers ---------------- */
struct DynBuf {
    ID3D11Buffer *buf = nullptr;
    ID3D11ShaderResourceView *srv = nullptr;
    UINT size = 0;
};
DynBuf g_vbuf, g_ibuf;
ID3D11Buffer *g_cb_vc, *g_cb_vx, *g_cb_px;

void *map_buf(DynBuf &b, UINT size, UINT bind, bool structured) {
    if (size > b.size) {
        release(b.srv);
        release(b.buf);
        b.size = std::max<UINT>(size * 2, 1 << 16);
        D3D11_BUFFER_DESC d{};
        d.ByteWidth = b.size;
        d.Usage = D3D11_USAGE_DYNAMIC;
        d.BindFlags = bind;
        d.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (structured) { d.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED; d.StructureByteStride = 16; }
        g_dev->CreateBuffer(&d, nullptr, &b.buf);
        if (structured) {
            D3D11_SHADER_RESOURCE_VIEW_DESC s{};
            s.Format = DXGI_FORMAT_UNKNOWN;
            s.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
            s.Buffer.NumElements = b.size / 16;
            g_dev->CreateShaderResourceView(b.buf, &s, &b.srv);
        }
    }
    D3D11_MAPPED_SUBRESOURCE m;
    g_ctx->Map(b.buf, 0, D3D11_MAP_WRITE_DISCARD, 0, &m);
    return m.pData;
}

ID3D11Buffer *make_cb(UINT size) {
    D3D11_BUFFER_DESC d{};
    d.ByteWidth = size;
    d.Usage = D3D11_USAGE_DYNAMIC;
    d.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    d.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    ID3D11Buffer *b = nullptr;
    g_dev->CreateBuffer(&d, nullptr, &b);
    return b;
}
void upload_cb(ID3D11Buffer *b, const void *data, size_t n) {
    D3D11_MAPPED_SUBRESOURCE m;
    g_ctx->Map(b, 0, D3D11_MAP_WRITE_DISCARD, 0, &m);
    memcpy(m.pData, data, n);
    g_ctx->Unmap(b, 0);
}

/* ---------------- render target cache ---------------- */
struct Surface {
    uint32_t ea = 0, fmt = 0, w = 0, h = 0;
    bool depth = false;
    ID3D11Texture2D *tex = nullptr;
    ID3D11RenderTargetView *rtv = nullptr;
    ID3D11DepthStencilView *dsv = nullptr;
    ID3D11ShaderResourceView *srv = nullptr;
    ID3D11Texture2D *copy = nullptr;            // snapshot for read-while-render
    ID3D11ShaderResourceView *copy_srv = nullptr;
    uint32_t last_frame = 0;
};

// The RSX lets a shader sample the surface it is rendering to; D3D11 does
// not. Sample a snapshot taken just before the draw instead.
ID3D11ShaderResourceView *snapshot(Surface &s) {
    if (!s.copy) {
        D3D11_TEXTURE2D_DESC d;
        s.tex->GetDesc(&d);
        d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        g_dev->CreateTexture2D(&d, nullptr, &s.copy);
        if (s.depth) {
            D3D11_SHADER_RESOURCE_VIEW_DESC sv{};
            sv.Format = s.fmt == 1 ? DXGI_FORMAT_R16_UNORM : DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
            sv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
            sv.Texture2D.MipLevels = 1;
            g_dev->CreateShaderResourceView(s.copy, &sv, &s.copy_srv);
        } else {
            g_dev->CreateShaderResourceView(s.copy, nullptr, &s.copy_srv);
        }
    }
    g_ctx->CopyResource(s.copy, s.tex);
    return s.copy_srv;
}
std::unordered_map<uint32_t, Surface> g_surfaces, g_depth_surfaces;   // by guest address

DXGI_FORMAT color_format(uint32_t f) {
    switch (f) {
    case 0x9: return DXGI_FORMAT_R8_UNORM;
    case 0xa: return DXGI_FORMAT_R8G8_UNORM;
    case 0xb: return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case 0xc: return DXGI_FORMAT_R32G32B32A32_FLOAT;
    case 0xd: return DXGI_FORMAT_R32_FLOAT;
    default: return DXGI_FORMAT_R8G8B8A8_UNORM;
    }
}

Surface *get_surface(uint32_t ea, uint32_t fmt, uint32_t w, uint32_t h, bool depth) {
    if (!w || !h) return nullptr;
    Surface &s = (depth ? g_depth_surfaces : g_surfaces)[ea];
    if (s.tex && s.fmt == fmt && s.depth == depth && s.w >= w && s.h >= h) { s.last_frame = g_frame; return &s; }
    uint32_t nw = std::max(w, s.fmt == fmt ? s.w : 0), nh = std::max(h, s.fmt == fmt ? s.h : 0);
    release(s.rtv); release(s.dsv); release(s.srv); release(s.tex); release(s.copy_srv); release(s.copy);
    s = Surface{};
    s.ea = ea, s.fmt = fmt, s.w = nw, s.h = nh, s.depth = depth, s.last_frame = g_frame;
    D3D11_TEXTURE2D_DESC d{};
    d.Width = nw, d.Height = nh, d.MipLevels = 1, d.ArraySize = 1, d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_DEFAULT;
    if (depth) {
        bool z16 = fmt == 1;
        d.Format = z16 ? DXGI_FORMAT_R16_TYPELESS : DXGI_FORMAT_R24G8_TYPELESS;
        d.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;
        g_dev->CreateTexture2D(&d, nullptr, &s.tex);
        D3D11_DEPTH_STENCIL_VIEW_DESC dv{};
        dv.Format = z16 ? DXGI_FORMAT_D16_UNORM : DXGI_FORMAT_D24_UNORM_S8_UINT;
        dv.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
        g_dev->CreateDepthStencilView(s.tex, &dv, &s.dsv);
        D3D11_SHADER_RESOURCE_VIEW_DESC sv{};
        sv.Format = z16 ? DXGI_FORMAT_R16_UNORM : DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
        sv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        sv.Texture2D.MipLevels = 1;
        g_dev->CreateShaderResourceView(s.tex, &sv, &s.srv);
    } else {
        d.Format = color_format(fmt);
        d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        g_dev->CreateTexture2D(&d, nullptr, &s.tex);
        g_dev->CreateRenderTargetView(s.tex, nullptr, &s.rtv);
        g_dev->CreateShaderResourceView(s.tex, nullptr, &s.srv);
    }
    LOG("rsx", "new %s surface at 0x%08x fmt %u %ux%u", depth ? "depth" : "color", ea, fmt, nw, nh);
    return &s;
}

uint32_t dma_location(uint32_t ctx) { return (ctx & 0xf) == 1 ? 1 : 0; }  // 0xfeed0000 local, 0xfeed0001 main

struct Bound {
    Surface *color[4] = {};
    Surface *depth = nullptr;
    uint32_t w = 0, h = 0;
};

Bound bind_surfaces() {
    Bound b;
    uint32_t fmt = reg(0x0208);
    uint32_t cfmt = fmt & 0x1f, zfmt = (fmt >> 5) & 7;
    b.w = (reg(0x0200) >> 16) + (reg(0x0200) & 0xffff);
    b.h = (reg(0x0204) >> 16) + (reg(0x0204) & 0xffff);
    uint32_t target = reg(0x0220);
    static const uint32_t off_m[4] = {0x0210, 0x0218, 0x0280, 0x0284};
    static const uint32_t dma_m[4] = {0x0184, 0x0188, 0x018c, 0x01b4};
    int mask = 0;
    switch (target) {
    case 0x01: mask = 1; break;
    case 0x02: mask = 2; break;
    case 0x13: mask = 3; break;
    case 0x17: mask = 7; break;
    case 0x1f: mask = 15; break;
    }
    ID3D11RenderTargetView *rtv[4] = {};
    for (int i = 0; i < 4; i++) {
        if (!(mask & (1 << i))) continue;
        uint32_t ea = ea_for(dma_location(reg(dma_m[i])), reg(off_m[i]));
        b.color[i] = get_surface(ea, cfmt, b.w, b.h, false);
        if (b.color[i]) rtv[i] = b.color[i]->rtv;
    }
    ID3D11DepthStencilView *dsv = nullptr;
    if (zfmt) {
        uint32_t ea = ea_for(dma_location(reg(0x0194)), reg(0x0214));
        b.depth = get_surface(ea, zfmt, b.w, b.h, true);
        if (b.depth) dsv = b.depth->dsv;
    }
    // A bound surface must not also be bound as a texture.
    ID3D11ShaderResourceView *none[16] = {};
    g_ctx->PSSetShaderResources(0, 16, none);
    g_ctx->OMSetRenderTargets(4, rtv, dsv);
    D3D11_VIEWPORT vp{0, 0, (float)b.w, (float)b.h, 0, 1};
    g_ctx->RSSetViewports(1, &vp);
    return b;
}

/* ---------------- textures ---------------- */
struct Tex {
    ID3D11Texture2D *tex = nullptr;
    ID3D11ShaderResourceView *srv = nullptr;
    uint64_t content = 0;
    uint32_t checked_frame = ~0u;
};
std::unordered_map<uint64_t, Tex> g_textures;

uint32_t swizzled_offset(uint32_t x, uint32_t y, uint32_t lw, uint32_t lh) {
    // Morton order up to the smaller dimension, then linear in the larger one.
    uint32_t off = 0, bit = 0, m = std::min(lw, lh);
    for (uint32_t i = 0; i < m; i++) {
        off |= ((x >> i) & 1) << bit++;
        off |= ((y >> i) & 1) << bit++;
    }
    if (lw > lh) off |= (x >> m) << bit;
    else off |= (y >> m) << bit;
    return off;
}

inline uint32_t log2u(uint32_t v) { uint32_t l = 0; while ((1u << l) < v) l++; return l; }

// Decode one 2D image (mip 0) into RGBA8, or pass DXT through. Returns D3D format.
DXGI_FORMAT decode(uint32_t ea, uint32_t fmt, uint32_t w, uint32_t h, uint32_t pitch, std::vector<uint8_t> &out, UINT &row) {
    uint32_t base = fmt & ~0x60u;
    bool linear = fmt & 0x20;
    if (base >= 0x86 && base <= 0x88) {
        uint32_t bs = base == 0x86 ? 8 : 16;
        uint32_t bw = (w + 3) / 4, bh = (h + 3) / 4;
        row = bw * bs;
        out.resize((size_t)row * bh);
        memcpy(out.data(), mem::ptr(ea), out.size());
        return base == 0x86 ? DXGI_FORMAT_BC1_UNORM : base == 0x87 ? DXGI_FORMAT_BC2_UNORM : DXGI_FORMAT_BC3_UNORM;
    }
    uint32_t bpp;
    switch (base) {
    case 0x81: bpp = 1; break;
    case 0x82: case 0x83: case 0x84: case 0x8b: case 0x94: case 0x97: case 0x9d: bpp = 2; break;
    case 0x9a: case 0x9b: bpp = base == 0x9a ? 8 : 16; break;
    default: bpp = 4; break;
    }
    if (!pitch || !linear) pitch = w * bpp;
    uint32_t lw = log2u(w), lh = log2u(h);
    const uint8_t *src = (const uint8_t *)mem::ptr(ea);
    bool hdr = base == 0x9a || base == 0x9b;
    row = w * (hdr ? (base == 0x9a ? 8 : 16) : 4);
    out.resize((size_t)row * h);
    for (uint32_t y = 0; y < h; y++)
        for (uint32_t x = 0; x < w; x++) {
            size_t o = linear ? (size_t)y * pitch + (size_t)x * bpp : (size_t)swizzled_offset(x, y, lw, lh) * bpp;
            const uint8_t *p = src + o;
            uint8_t *d = &out[(size_t)y * row + (size_t)x * (row / w)];
            if (hdr) {  // big-endian halves/floats -> little-endian
                uint32_t n = base == 0x9a ? 4 : 4, cs = base == 0x9a ? 2 : 4;
                for (uint32_t c = 0; c < n; c++)
                    for (uint32_t k = 0; k < cs; k++) d[c * cs + k] = p[c * cs + (cs - 1 - k)];
                continue;
            }
            uint8_t r, g, b, a;
            uint16_t v16 = (uint16_t)(p[0] << 8 | (bpp > 1 ? p[1] : 0));
            switch (base) {
            case 0x81: r = g = b = a = p[0]; break;
            case 0x82: a = (v16 >> 15) ? 255 : 0; r = ((v16 >> 10) & 31) * 255 / 31; g = ((v16 >> 5) & 31) * 255 / 31; b = (v16 & 31) * 255 / 31; break;
            case 0x83: a = (v16 >> 12) * 17; r = ((v16 >> 8) & 15) * 17; g = ((v16 >> 4) & 15) * 17; b = (v16 & 15) * 17; break;
            case 0x84: a = 255; r = (v16 >> 11) * 255 / 31; g = ((v16 >> 5) & 63) * 255 / 63; b = (v16 & 31) * 255 / 31; break;
            case 0x8b: r = p[0]; g = p[1]; b = 0; a = 255; break;
            case 0x94: r = g = b = p[0]; a = 255; break;
            case 0x9e: a = 255; r = p[1]; g = p[2]; b = p[3]; break;
            default: a = p[0]; r = p[1]; g = p[2]; b = p[3]; break;  // A8R8G8B8 and friends
            }
            d[0] = r, d[1] = g, d[2] = b, d[3] = a;
        }
    return base == 0x9a ? DXGI_FORMAT_R16G16B16A16_FLOAT : base == 0x9b ? DXGI_FORMAT_R32G32B32A32_FLOAT : DXGI_FORMAT_R8G8B8A8_UNORM;
}

uint32_t image_size(uint32_t fmt, uint32_t w, uint32_t h, uint32_t pitch) {
    uint32_t base = fmt & ~0x60u;
    if (base >= 0x86 && base <= 0x88) return ((w + 3) / 4) * ((h + 3) / 4) * (base == 0x86 ? 8 : 16);
    uint32_t bpp = (base == 0x81) ? 1 : (base == 0x82 || base == 0x83 || base == 0x84 || base == 0x8b || base == 0x94) ? 2 : base == 0x9a ? 8 : base == 0x9b ? 16 : 4;
    return (fmt & 0x20) && pitch ? pitch * h : w * h * bpp;
}

ID3D11ShaderResourceView *get_texture(int unit, TexDim &dim) {
    uint32_t b = 0x1a00 + unit * 32;
    uint32_t off = reg(b), fmtreg = reg(b + 4), rect = reg(b + 0x18);
    uint32_t loc = (fmtreg & 3) == 2 ? 1 : 0;
    uint32_t fmt = (fmtreg >> 8) & 0xff, mips = std::max(1u, fmtreg >> 16);
    bool cube = fmtreg & 4;
    uint32_t w = rect >> 16, h = rect & 0xffff;
    uint32_t pitch = reg(0x1840 + unit * 4) & 0xfffff;
    dim = cube ? TEX_CUBE : TEX_2D;
    uint32_t ea = ea_for(loc, off);
    if (!ea || !w || !h) return nullptr;
    // Render-to-texture: use the surface directly.
    auto s = g_surfaces.find(ea);
    if (s != g_surfaces.end() && !cube) return s->second.srv;

    uint64_t key = fnv(&ea, 4, fnv(&fmtreg, 4, fnv(&rect, 4, fnv(&pitch, 4))));
    Tex &t = g_textures[key];
    uint32_t size = image_size(fmt, w, h, pitch);
    if (t.srv && t.checked_frame == g_frame) return t.srv;
    // Detect CPU-side changes: hash up to 64 KiB spread over the image.
    uint64_t hv = 1469598103934665603ull;
    const uint8_t *p = (const uint8_t *)mem::ptr(ea);
    if (size <= 65536) hv = fnv(p, size);
    else for (uint32_t k = 0; k < 1024; k++) hv = fnv(p + (uint64_t)size * k / 1024, 64, hv);
    t.checked_frame = g_frame;
    if (t.srv && t.content == hv) return t.srv;
    release(t.srv);
    release(t.tex);
    t.content = hv;

    int faces = cube ? 6 : 1;
    uint32_t face_stride = 0;
    if (cube) {  // each face holds its full mip chain, 128-byte aligned
        for (uint32_t m = 0, mw = w, mh = h; m < mips; m++, mw = std::max(1u, mw / 2), mh = std::max(1u, mh / 2))
            face_stride += image_size(fmt, mw, mh, (fmt & 0x20) ? pitch : 0);
        face_stride = (face_stride + 127) & ~127u;
    }
    std::vector<std::vector<uint8_t>> data(faces);
    D3D11_SUBRESOURCE_DATA sd[6];
    DXGI_FORMAT df = DXGI_FORMAT_UNKNOWN;
    for (int f = 0; f < faces; f++) {
        UINT row;
        df = decode(ea + f * face_stride, fmt, w, h, pitch, data[f], row);
        sd[f] = {data[f].data(), row, 0};
    }
    D3D11_TEXTURE2D_DESC d{};
    d.Width = w, d.Height = h, d.MipLevels = 1, d.ArraySize = faces, d.Format = df;
    d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_IMMUTABLE;
    d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    d.MiscFlags = cube ? D3D11_RESOURCE_MISC_TEXTURECUBE : 0;
    if (FAILED(g_dev->CreateTexture2D(&d, sd, &t.tex))) {
        LOG("rsx", "texture create failed fmt 0x%x %ux%u", fmt, w, h);
        return nullptr;
    }
    g_dev->CreateShaderResourceView(t.tex, nullptr, &t.srv);
    return t.srv;
}

std::unordered_map<uint64_t, ID3D11SamplerState *> g_samplers;
ID3D11SamplerState *get_sampler(int unit) {
    uint32_t b = 0x1a00 + unit * 32;
    uint32_t addr = reg(b + 8), filt = reg(b + 0x14);
    uint64_t key = ((uint64_t)addr << 32) | (filt & 0xffff0000);
    auto it = g_samplers.find(key);
    if (it != g_samplers.end()) return it->second;
    auto wrap = [](uint32_t m) {
        switch (m) {
        case 1: return D3D11_TEXTURE_ADDRESS_WRAP;
        case 2: return D3D11_TEXTURE_ADDRESS_MIRROR;
        case 4: return D3D11_TEXTURE_ADDRESS_BORDER;
        case 6: case 7: case 8: return D3D11_TEXTURE_ADDRESS_MIRROR_ONCE;
        default: return D3D11_TEXTURE_ADDRESS_CLAMP;
        }
    };
    D3D11_SAMPLER_DESC d{};
    bool lin = ((filt >> 24) & 0xff) != 1;
    d.Filter = lin ? D3D11_FILTER_MIN_MAG_MIP_LINEAR : D3D11_FILTER_MIN_MAG_MIP_POINT;
    d.AddressU = wrap(addr & 0xf);
    d.AddressV = wrap((addr >> 8) & 0xf);
    d.AddressW = wrap((addr >> 16) & 0xf);
    d.MaxLOD = D3D11_FLOAT32_MAX;
    d.ComparisonFunc = D3D11_COMPARISON_NEVER;
    ID3D11SamplerState *s = nullptr;
    g_dev->CreateSamplerState(&d, &s);
    g_samplers[key] = s;
    return s;
}

/* ---------------- fixed-function state ---------------- */
D3D11_BLEND blend_factor(uint32_t f, bool alpha) {
    switch (f) {
    case 0: return D3D11_BLEND_ZERO;
    case 1: return D3D11_BLEND_ONE;
    case 0x300: return alpha ? D3D11_BLEND_SRC_ALPHA : D3D11_BLEND_SRC_COLOR;
    case 0x301: return alpha ? D3D11_BLEND_INV_SRC_ALPHA : D3D11_BLEND_INV_SRC_COLOR;
    case 0x302: return D3D11_BLEND_SRC_ALPHA;
    case 0x303: return D3D11_BLEND_INV_SRC_ALPHA;
    case 0x304: return D3D11_BLEND_DEST_ALPHA;
    case 0x305: return D3D11_BLEND_INV_DEST_ALPHA;
    case 0x306: return alpha ? D3D11_BLEND_DEST_ALPHA : D3D11_BLEND_DEST_COLOR;
    case 0x307: return alpha ? D3D11_BLEND_INV_DEST_ALPHA : D3D11_BLEND_INV_DEST_COLOR;
    case 0x308: return D3D11_BLEND_SRC_ALPHA_SAT;
    case 0x8001: case 0x8003: return D3D11_BLEND_BLEND_FACTOR;
    case 0x8002: case 0x8004: return D3D11_BLEND_INV_BLEND_FACTOR;
    default: return D3D11_BLEND_ONE;
    }
}
D3D11_BLEND_OP blend_op(uint32_t e) {
    switch (e) {
    case 0x8007: return D3D11_BLEND_OP_MIN;
    case 0x8008: return D3D11_BLEND_OP_MAX;
    case 0x800a: return D3D11_BLEND_OP_SUBTRACT;
    case 0x800b: return D3D11_BLEND_OP_REV_SUBTRACT;
    default: return D3D11_BLEND_OP_ADD;
    }
}
D3D11_COMPARISON_FUNC cmp(uint32_t f) {
    uint32_t v = (f & 0xf) + 1;
    return (D3D11_COMPARISON_FUNC)(v >= 1 && v <= 8 ? v : 8);
}
D3D11_STENCIL_OP sop(uint32_t o) {
    switch (o) {
    case 0: return D3D11_STENCIL_OP_ZERO;
    case 0x1e00: return D3D11_STENCIL_OP_KEEP;
    case 0x1e01: return D3D11_STENCIL_OP_REPLACE;
    case 0x1e02: return D3D11_STENCIL_OP_INCR_SAT;
    case 0x1e03: return D3D11_STENCIL_OP_DECR_SAT;
    case 0x150a: return D3D11_STENCIL_OP_INVERT;
    case 0x8507: return D3D11_STENCIL_OP_INCR;
    case 0x8508: return D3D11_STENCIL_OP_DECR;
    default: return D3D11_STENCIL_OP_KEEP;
    }
}

std::unordered_map<uint64_t, ID3D11BlendState *> g_blend;
std::unordered_map<uint64_t, ID3D11DepthStencilState *> g_depth;
std::unordered_map<uint64_t, ID3D11RasterizerState *> g_raster;

void apply_state() {
    // blend
    uint32_t be = reg(0x0310) & 1, sf = reg(0x0314), df = reg(0x0318), eq = reg(0x0320), mask = reg(0x0324);
    uint64_t bk = fnv(&be, 4, fnv(&sf, 4, fnv(&df, 4, fnv(&eq, 4, fnv(&mask, 4)))));
    auto &bs = g_blend[bk];
    if (!bs) {
        D3D11_BLEND_DESC d{};
        d.IndependentBlendEnable = FALSE;
        auto &t = d.RenderTarget[0];
        t.BlendEnable = be;
        t.SrcBlend = blend_factor(sf & 0xffff, false);
        t.DestBlend = blend_factor(df & 0xffff, false);
        t.BlendOp = blend_op(eq & 0xffff);
        t.SrcBlendAlpha = blend_factor(sf >> 16, true);
        t.DestBlendAlpha = blend_factor(df >> 16, true);
        t.BlendOpAlpha = blend_op(eq >> 16);
        t.RenderTargetWriteMask = ((mask >> 16) & 1 ? 1 : 0) | ((mask >> 8) & 1 ? 2 : 0) | (mask & 1 ? 4 : 0) | ((mask >> 24) & 1 ? 8 : 0);
        g_dev->CreateBlendState(&d, &bs);
    }
    uint32_t bc = reg(0x031c);
    float bcf[4] = {((bc >> 16) & 255) / 255.f, ((bc >> 8) & 255) / 255.f, (bc & 255) / 255.f, (bc >> 24) / 255.f};
    g_ctx->OMSetBlendState(bs, bcf, 0xffffffff);

    // depth / stencil
    uint32_t dz[] = {reg(0x0a74) & 1, reg(0x0a6c), reg(0x0a70) & 1, reg(0x0328) & 1, reg(0x032c), reg(0x0330),
                     reg(0x0338), reg(0x033c), reg(0x0340), reg(0x0344)};
    auto &ds = g_depth[fnv(dz, sizeof dz)];
    if (!ds) {
        D3D11_DEPTH_STENCIL_DESC d{};
        static bool nodepth = getenv("KZ2_NO_DEPTH_STENCIL");
        if (nodepth) dz[0] = dz[3] = 0;
        d.DepthEnable = dz[0];
        d.DepthFunc = cmp(dz[1]);
        d.DepthWriteMask = dz[2] ? D3D11_DEPTH_WRITE_MASK_ALL : D3D11_DEPTH_WRITE_MASK_ZERO;
        d.StencilEnable = dz[3];
        d.StencilWriteMask = (UINT8)dz[4];
        d.StencilReadMask = (UINT8)dz[6];
        d.FrontFace = {sop(dz[7]), sop(dz[8]), sop(dz[9]), cmp(dz[5])};
        d.BackFace = d.FrontFace;
        g_dev->CreateDepthStencilState(&d, &ds);
    }
    g_ctx->OMSetDepthStencilState(ds, reg(0x0334) & 0xff);

    // rasterizer: culling off for now (window-origin flips change winding), scissor on
    uint32_t sc = 1;
    auto &rs = g_raster[sc];
    if (!rs) {
        D3D11_RASTERIZER_DESC d{};
        d.FillMode = D3D11_FILL_SOLID;
        d.CullMode = D3D11_CULL_NONE;
        d.DepthClipEnable = TRUE;
        d.ScissorEnable = TRUE;
        g_dev->CreateRasterizerState(&d, &rs);
    }
    g_ctx->RSSetState(rs);
    uint32_t sh = reg(0x08c0), sv = reg(0x08c4);
    D3D11_RECT r{(LONG)(sh & 0xffff), (LONG)(sv & 0xffff), (LONG)((sh & 0xffff) + (sh >> 16)), (LONG)((sv & 0xffff) + (sv >> 16))};
    if (!(sh >> 16) || !(sv >> 16)) r = {0, 0, 8192, 8192};
    g_ctx->RSSetScissorRects(1, &r);
}

/* ---------------- vertex fetch ---------------- */
inline float h2f(uint16_t h) {
    uint32_t s = (h >> 15) & 1, e = (h >> 10) & 31, m = h & 1023, f;
    if (!e) { if (!m) f = s << 31; else { e = 127 - 15 + 1; while (!(m & 1024)) { m <<= 1; e--; } m &= 1023; f = (s << 31) | (e << 23) | (m << 13); } }
    else if (e == 31) f = (s << 31) | 0x7f800000 | (m << 13);
    else f = (s << 31) | ((e - 15 + 127) << 23) | (m << 13);
    float r; memcpy(&r, &f, 4); return r;
}

void fetch_vertex(float *out, uint32_t index) {
    uint32_t mask = reg(0x1ff0), base_off = reg(0x1738), base_idx = reg(0x173c);
    for (int a = 0; a < 16; a++) {
        float *v = out + a * 4;
        uint32_t fmt = reg(0x1740 + a * 4);
        uint32_t type = fmt & 0xf, size = (fmt >> 4) & 0xf, stride = (fmt >> 8) & 0xff;
        if (!(mask & (1 << a)) || !size) { memcpy(v, g_vattr[a], 16); continue; }
        uint32_t off = reg(0x1680 + a * 4);
        uint32_t ea = ea_for(off >> 31, (off & 0x7fffffff) + base_off) + (base_idx + index) * stride;
        v[0] = v[1] = v[2] = 0, v[3] = 1;
        if (type == 6) {  // CMP: 11:11:10 signed normalised
            uint32_t p = ld32(ea);
            int32_t x = (int32_t)(p << 21) >> 21, y = (int32_t)(p << 10) >> 21, z = (int32_t)p >> 22;
            v[0] = x / 1023.f, v[1] = y / 1023.f, v[2] = z / 511.f;
            continue;
        }
        for (uint32_t c = 0; c < size && c < 4; c++) {
            switch (type) {
            case 1: v[c] = (int16_t)ld16(ea + 2 * c) / 32767.f; break;
            case 2: { uint32_t u = ld32(ea + 4 * c); memcpy(&v[c], &u, 4); break; }
            case 3: v[c] = h2f(ld16(ea + 2 * c)); break;
            case 4: v[c] = ld8(ea + c) / 255.f; break;
            case 5: v[c] = (float)(int16_t)ld16(ea + 2 * c); break;
            case 7: v[c] = (float)ld8(ea + c); break;
            }
        }
    }
}

// Build a list topology from the RSX primitive. Returns D3D topology.
D3D11_PRIMITIVE_TOPOLOGY gen_indices(uint32_t prim, const uint32_t *idx, uint32_t n, std::vector<uint32_t> &out) {
    switch (prim) {
    case 1: out.insert(out.end(), idx, idx + n); return D3D11_PRIMITIVE_TOPOLOGY_POINTLIST;
    case 2: out.insert(out.end(), idx, idx + (n & ~1u)); return D3D11_PRIMITIVE_TOPOLOGY_LINELIST;
    case 3: case 4:
        for (uint32_t i = 0; i + 1 < n; i++) { out.push_back(idx[i]); out.push_back(idx[i + 1]); }
        if (prim == 3 && n > 2) { out.push_back(idx[n - 1]); out.push_back(idx[0]); }
        return D3D11_PRIMITIVE_TOPOLOGY_LINELIST;
    case 5: out.insert(out.end(), idx, idx + n - n % 3); break;
    case 6:
        for (uint32_t i = 0; i + 2 < n; i++) {
            if (i & 1) { out.push_back(idx[i + 1]); out.push_back(idx[i]); out.push_back(idx[i + 2]); }
            else { out.push_back(idx[i]); out.push_back(idx[i + 1]); out.push_back(idx[i + 2]); }
        }
        break;
    case 7: case 10:
        for (uint32_t i = 1; i + 1 < n; i++) { out.push_back(idx[0]); out.push_back(idx[i]); out.push_back(idx[i + 1]); }
        break;
    case 8:
        for (uint32_t i = 0; i + 3 < n; i += 4) {
            uint32_t q[6] = {idx[i], idx[i + 1], idx[i + 2], idx[i], idx[i + 2], idx[i + 3]};
            out.insert(out.end(), q, q + 6);
        }
        break;
    case 9:
        for (uint32_t i = 0; i + 3 < n; i += 2) {
            uint32_t q[6] = {idx[i], idx[i + 1], idx[i + 3], idx[i], idx[i + 3], idx[i + 2]};
            out.insert(out.end(), q, q + 6);
        }
        break;
    }
    return D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
}

/* ---------------- shaders for a draw ---------------- */
ID3D11VertexShader *vertex_shader() {
    uint32_t start = reg(0x1ea0) & 511;
    uint64_t h = 1469598103934665603ull;
    for (uint32_t i = start; i < 512; i++) {
        h = fnv(&vp_ucode[i * 4], 16, h);
        if (vp_ucode[i * 4 + 3] & 1) break;
    }
    auto it = g_vs.find(h);
    if (it != g_vs.end()) return it->second;
    VpInfo vi = translate_vp(start);
    ID3D11VertexShader *vs = nullptr;
    if (ID3DBlob *b = compile(vi.hlsl, "vs_5_0", h)) {
        g_dev->CreateVertexShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &vs);
        b->Release();
    }
    g_vs[h] = vs;
    return vs;
}

PsEntry *pixel_shader(uint32_t fp_ea, const uint8_t texdim[16]) {
    FpScan sc = scan_fp(fp_ea);
    uint32_t control = reg(0x1d60);
    uint32_t alpha = (reg(0x0304) & 1) ? reg(0x0308) : 0;
    uint64_t h = fnv(texdim, 16, fnv(&control, 4, fnv(&alpha, 4, sc.hash)));
    auto it = g_ps.find(h);
    if (it != g_ps.end()) return &it->second;
    FpKey key{fp_ea, control, {}, alpha};
    memcpy(key.texdim, texdim, 16);
    FpInfo fi = translate_fp(key);
    PsEntry e{nullptr, sc.consts};
    if (ID3DBlob *b = compile(fi.hlsl, "ps_5_0", h)) {
        g_dev->CreatePixelShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &e.ps);
        b->Release();
    }
    return &(g_ps[h] = e);
}

void draw() {
    if (!g_ok || g_ranges.empty()) return;
    g_draws_this_frame++;
    // debug: KZ2_SKIP_DRAWS=a,b,c skips those draw indices in every frame
    static std::vector<int> skip = [] {
        std::vector<int> v;
        if (const char *e = getenv("KZ2_SKIP_DRAWS"))
            for (const char *p = e; *p;) { v.push_back(atoi(p)); while (*p && *p != ',') p++; if (*p) p++; }
        return v;
    }();
    for (int k : skip) if (k == g_draws_this_frame) return;
    Bound b = bind_surfaces();
    if (!b.w) return;

    // vertices + indices
    std::vector<uint32_t> src_idx;
    uint32_t vmin = ~0u, vmax = 0;
    if (g_indexed) {
        uint32_t loc = reg(0x1820) & 0xf, type = (reg(0x1820) >> 4) & 0xf;
        uint32_t ea = ea_for(loc ? 1 : 0, reg(0x181c));
        bool restart = reg(0x1dac) & 1;
        uint32_t rval = reg(0x1db0);
        for (auto &r : g_ranges)
            for (uint32_t i = 0; i < r.count; i++) {
                uint32_t v = type ? ld16(ea + 2 * (r.first + i)) : ld32(ea + 4 * (r.first + i));
                if (restart && v == rval) { src_idx.push_back(~0u); continue; }
                src_idx.push_back(v);
                vmin = std::min(vmin, v), vmax = std::max(vmax, v);
            }
    } else {
        for (auto &r : g_ranges) {
            vmin = std::min(vmin, r.first), vmax = std::max(vmax, r.first + r.count - 1);
            for (uint32_t i = 0; i < r.count; i++) src_idx.push_back(r.first + i);
        }
    }
    if (vmin > vmax || vmax - vmin > 4 * 1024 * 1024) return;
    uint32_t nv = vmax - vmin + 1;
    float *vd = (float *)map_buf(g_vbuf, nv * 256, D3D11_BIND_SHADER_RESOURCE, true);
    for (uint32_t i = 0; i < nv; i++) fetch_vertex(vd + i * 64, vmin + i);
    g_ctx->Unmap(g_vbuf.buf, 0);

    std::vector<uint32_t> list;
    D3D11_PRIMITIVE_TOPOLOGY topo = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    // split at restart markers / range boundaries for strip-like primitives
    std::vector<uint32_t> run;
    size_t at = 0;
    auto flush_run = [&] {
        for (auto &v : run) v -= vmin;
        if (!run.empty()) topo = gen_indices(g_prim, run.data(), (uint32_t)run.size(), list);
        run.clear();
    };
    for (auto &r : g_ranges) {
        for (uint32_t i = 0; i < r.count && at < src_idx.size(); i++, at++) {
            if (src_idx[at] == ~0u) { flush_run(); continue; }
            run.push_back(src_idx[at]);
        }
        flush_run();
    }
    if (list.empty()) return;
    uint32_t *id = (uint32_t *)map_buf(g_ibuf, (UINT)list.size() * 4, D3D11_BIND_INDEX_BUFFER, false);
    memcpy(id, list.data(), list.size() * 4);
    g_ctx->Unmap(g_ibuf.buf, 0);

    // shaders
    ID3D11VertexShader *vs = vertex_shader();
    uint32_t sp = reg(0x08e4);
    uint32_t fp_ea = ea_for((sp & 3) == 2 ? 1 : 0, sp & ~3u);
    uint8_t texdim[16] = {};
    ID3D11ShaderResourceView *srv[16] = {};
    ID3D11SamplerState *smp[16] = {};
    for (int t = 0; t < 16; t++) {
        if (!(reg(0x1a0c + t * 32) & 0x80000000)) continue;
        TexDim d;
        srv[t] = get_texture(t, d);
        texdim[t] = d;
        if ((reg(0x1a04 + t * 32) >> 8) & 0x40) texdim[t] |= TEX_UNNORM;
        smp[t] = get_sampler(t);
    }
    PsEntry *ps = pixel_shader(fp_ea, texdim);
    if (!vs || !ps->ps) return;

    // constants
    if (vp_consts_dirty) { upload_cb(g_cb_vc, vp_consts, sizeof vp_consts); vp_consts_dirty = false; }
    float vx[12] = {regf(0x0a30), regf(0x0a34), regf(0x0a38), regf(0x0a3c), regf(0x0a20), regf(0x0a24),
                    regf(0x0a28), regf(0x0a2c), (float)b.w, (float)b.h, 0, 0};
    bool origin_bottom = (reg(0x1d88) >> 12) & 0xf;
    // Window-origin "bottom" (0x1d88 bit 12) maps to D3D top-down here; verified against
    // the font atlas, which must come out upright for text lookups to work.
    origin_bottom = !origin_bottom;
    if (getenv("KZ2_FLIP_Y")) origin_bottom = !origin_bottom;
    vx[10] = origin_bottom ? 1.f : 0.f;
    upload_cb(g_cb_vx, vx, sizeof vx);
    struct { float alpha_ref[4]; float wpos[4]; float texscale[16][4]; uint32_t fc[128 * 4]; } px{};
    for (int t = 0; t < 16; t++) {
        uint32_t rect = reg(0x1a18 + t * 32);
        px.texscale[t][0] = (rect >> 16) ? 1.f / (rect >> 16) : 1.f;
        px.texscale[t][1] = (rect & 0xffff) ? 1.f / (rect & 0xffff) : 1.f;
    }
    uint32_t ar = reg(0x030c);
    float arf;
    if (ar <= 255) arf = ar / 255.f; else memcpy(&arf, &ar, 4);
    px.alpha_ref[0] = arf;
    px.wpos[0] = origin_bottom ? 1.f : 0.f;
    px.wpos[1] = (float)(reg(0x1d88) & 0xfff);
    for (size_t k = 0; k < ps->consts.size(); k++)
        for (int c = 0; c < 4; c++) px.fc[k * 4 + c] = fp_const_word(fp_ea + ps->consts[k] + 4 * c);
    upload_cb(g_cb_px, &px, 32 + 256 + ps->consts.size() * 16);
    static int log_frame = getenv("KZ2_LOG_DRAWS") ? atoi(getenv("KZ2_LOG_DRAWS")) : -1;
    if ((int)g_frame == log_frame) {
        float f[8];
        memcpy(f, px.fc, 32);
        uint64_t psh = 0;
        for (auto &[k, v] : g_ps) if (&v == ps) psh = k;
        LOG("draw", "#%d rt=%x tex0=%x ps=%016llx cmask=%08x blend=%x/%x/%x fc0=(%g %g %g %g) fc1=(%g %g %g %g) n=%zu",
            g_draws_this_frame, b.color[0] ? b.color[0]->ea : 0, srv[0] ? ea_for((reg(0x1a04) & 3) == 2, reg(0x1a00)) : 0,
            (unsigned long long)psh, reg(0x324), reg(0x310), reg(0x314), reg(0x318), f[0], f[1], f[2], f[3], f[4], f[5], f[6], f[7], list.size());
        static int attr_draw = getenv("KZ2_LOG_ATTRS") ? atoi(getenv("KZ2_LOG_ATTRS")) : -1;
        if (g_draws_this_frame == attr_draw) {
            float vv[64];
            fetch_vertex(vv, vmin);
            for (int t = 0; t < 16; t++)
                if (reg(0x1a0c + t * 32) & 0x80000000)
                    LOG("draw", "   tex%d ea=%x fmt=%08x rect=%08x %s", t, ea_for((reg(0x1a04 + t * 32) & 3) == 2, reg(0x1a00 + t * 32)),
                        reg(0x1a04 + t * 32), reg(0x1a18 + t * 32), g_surfaces.count(ea_for((reg(0x1a04 + t * 32) & 3) == 2, reg(0x1a00 + t * 32))) ? "(render target)" : "");
            for (int a = 0; a < 16; a++) if (reg(0x1ff0) & (1 << a))
                LOG("draw", "   attr%d fmt=%08x off=%08x: %g %g %g %g", a, reg(0x1740 + a * 4), reg(0x1680 + a * 4), vv[a * 4], vv[a * 4 + 1], vv[a * 4 + 2], vv[a * 4 + 3]);
        }
    }

    apply_state();
    g_ctx->VSSetShader(vs, nullptr, 0);
    g_ctx->PSSetShader(ps->ps, nullptr, 0);
    ID3D11Buffer *vcb[2] = {g_cb_vc, g_cb_vx};
    g_ctx->VSSetConstantBuffers(0, 2, vcb);
    g_ctx->PSSetConstantBuffers(0, 1, &g_cb_px);
    g_ctx->VSSetShaderResources(0, 1, &g_vbuf.srv);
    // sampling a surface that is also being rendered to: use a snapshot
    for (auto &s : srv) {
        for (auto *c : b.color) if (s && c && s == c->srv) s = snapshot(*c);
        if (s && b.depth && s == b.depth->srv) s = snapshot(*b.depth);
    }
    g_ctx->PSSetShaderResources(0, 16, srv);
    g_ctx->PSSetSamplers(0, 16, smp);
    g_ctx->IASetIndexBuffer(g_ibuf.buf, DXGI_FORMAT_R32_UINT, 0);
    g_ctx->IASetPrimitiveTopology(topo);
    g_ctx->DrawIndexed((UINT)list.size(), 0, 0);
}

/* ---------------- present ---------------- */
ID3D11Texture2D *g_cpu_tex;
ID3D11ShaderResourceView *g_cpu_srv;
ID3D11SamplerState *g_present_smp;

const char *PRESENT_HLSL =
    "Texture2D t : register(t0); SamplerState s : register(s0);\n"
    "cbuffer P : register(b0) { float4 uvscale; };\n"
    "struct V { float4 p : SV_Position; float2 uv : TEXCOORD0; };\n"
    "V vs(uint id : SV_VertexID) { V o; float2 q = float2((id << 1) & 2, id & 2); o.uv = q * uvscale.xy;\n"
    "  o.p = float4(q * float2(2, -2) + float2(-1, 1), 0, 1); return o; }\n"
    "float4 ps(V i) : SV_Target { return float4(t.Sample(s, i.uv).rgb, 1); }\n";
ID3D11Buffer *g_cb_present;

bool create_device() {
    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount = 1;
    sd.BufferDesc.Width = 1280, sd.BufferDesc.Height = 720;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = g_hwnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;  // blt model: stretches to the window, capturable
    D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;
    UINT flags = getenv("KZ2_D3D_DEBUG") ? D3D11_CREATE_DEVICE_DEBUG : 0;
    HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, &fl, 1,
                                               D3D11_SDK_VERSION, &sd, &g_swap, &g_dev, nullptr, &g_ctx);
    if (FAILED(hr)) { LOG("rsx", "D3D11 device creation failed (0x%08lx)", hr); return false; }
    ID3D11Texture2D *bb = nullptr;
    g_swap->GetBuffer(0, __uuidof(ID3D11Texture2D), (void **)&bb);
    g_dev->CreateRenderTargetView(bb, nullptr, &g_backbuffer);
    bb->Release();
    std::string src = PRESENT_HLSL;
    ID3DBlob *code = nullptr, *err = nullptr;
    D3DCompile(src.data(), src.size(), nullptr, nullptr, nullptr, "vs", "vs_5_0", 0, 0, &code, &err);
    g_dev->CreateVertexShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &g_present_vs);
    code->Release();
    D3DCompile(src.data(), src.size(), nullptr, nullptr, nullptr, "ps", "ps_5_0", 0, 0, &code, &err);
    g_dev->CreatePixelShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &g_present_ps);
    code->Release();
    D3D11_SAMPLER_DESC smp{};
    smp.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    smp.AddressU = smp.AddressV = smp.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    smp.MaxLOD = D3D11_FLOAT32_MAX;
    g_dev->CreateSamplerState(&smp, &g_present_smp);
    g_cb_vc = make_cb(sizeof vp_consts);
    g_cb_vx = make_cb(64);
    g_cb_px = make_cb(32 + 256 + 128 * 16);
    g_cb_present = make_cb(16);
    for (auto &a : g_vattr) a[0] = a[1] = a[2] = 0, a[3] = 1;
    LOG("rsx", "Direct3D 11 renderer ready");
    return true;
}
} // namespace

uint32_t local_ea(uint32_t off) { return mem::LOCAL_BASE + off; }
uint32_t ea_for(uint32_t location, uint32_t off) { return location ? io_ea(off) : local_ea(off); }

void init(void *hwnd) {
    g_hwnd = (HWND)hwnd;
    g_ok = create_device();
}

void clear(uint32_t mask) {
    if (!g_ok) return;
    Bound b = bind_surfaces();
    if (mask & 0xf0) {
        uint32_t c = reg(0x1d90);
        float f[4] = {((c >> 16) & 255) / 255.f, ((c >> 8) & 255) / 255.f, (c & 255) / 255.f, (c >> 24) / 255.f};
        for (auto *s : b.color) if (s) g_ctx->ClearRenderTargetView(s->rtv, f);
    }
    if ((mask & 3) && b.depth) {
        uint32_t z = reg(0x1d8c);
        UINT fl = (mask & 1 ? D3D11_CLEAR_DEPTH : 0) | (mask & 2 ? D3D11_CLEAR_STENCIL : 0);
        float depth = b.depth->fmt == 1 ? (z & 0xffff) / 65535.f : (z >> 8) / 16777215.f;
        g_ctx->ClearDepthStencilView(b.depth->dsv, fl, depth, (UINT8)(z & 0xff));
    }
}

void begin_end(uint32_t prim) {
    if (prim) {
        g_prim = prim;
        g_ranges.clear();
        g_indexed = false;
    } else {
        draw();
        g_ranges.clear();
    }
}
void draw_arrays(uint32_t first, uint32_t count) { g_indexed = false; g_ranges.push_back({first, count}); }
void draw_indexed(uint32_t first, uint32_t count) { g_indexed = true; g_ranges.push_back({first, count}); }

// Debug: write every 8-bit colour surface to rt_<address>.bmp
void dump_surfaces() {
    for (auto &[ea, s] : g_surfaces) {
        if (color_format(s.fmt) != DXGI_FORMAT_R8G8B8A8_UNORM) continue;
        D3D11_TEXTURE2D_DESC d;
        s.tex->GetDesc(&d);
        d.Usage = D3D11_USAGE_STAGING, d.BindFlags = 0, d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ID3D11Texture2D *st = nullptr;
        g_dev->CreateTexture2D(&d, nullptr, &st);
        g_ctx->CopyResource(st, s.tex);
        D3D11_MAPPED_SUBRESOURCE m;
        g_ctx->Map(st, 0, D3D11_MAP_READ, 0, &m);
        char path[64];
        snprintf(path, sizeof path, "rt_%08x.bmp", ea);
        if (FILE *f = fopen(path, "wb")) {
            uint32_t sz = d.Width * d.Height * 4;
            uint8_t hdr[54] = {'B', 'M'};
            *(uint32_t *)&hdr[2] = 54 + sz, *(uint32_t *)&hdr[10] = 54, *(uint32_t *)&hdr[14] = 40;
            *(int32_t *)&hdr[18] = d.Width, *(int32_t *)&hdr[22] = -(int32_t)d.Height;
            *(uint16_t *)&hdr[26] = 1, *(uint16_t *)&hdr[28] = 32;
            fwrite(hdr, 1, 54, f);
            std::vector<uint8_t> row(d.Width * 4);
            for (UINT y = 0; y < d.Height; y++) {
                const uint8_t *p = (const uint8_t *)m.pData + y * m.RowPitch;
                for (UINT x = 0; x < d.Width; x++) row[x * 4] = p[x * 4 + 2], row[x * 4 + 1] = p[x * 4 + 1], row[x * 4 + 2] = p[x * 4], row[x * 4 + 3] = 255;
                fwrite(row.data(), 1, row.size(), f);
            }
            fclose(f);
        }
        g_ctx->Unmap(st, 0);
        st->Release();
    }
    LOG("rsx", "dumped %zu surfaces at frame %u", g_surfaces.size(), g_frame);
}

void flip(uint32_t offset, uint32_t width, uint32_t height, uint32_t pitch) {
    if (!g_ok) return;
    g_frame++;
    static int dump_at = getenv("KZ2_DUMP_RT") ? atoi(getenv("KZ2_DUMP_RT")) : -1;
    if ((int)g_frame == dump_at) dump_surfaces();
    if (getenv("KZ2_TRACE_DRAWS") && g_frame % 60 == 0) LOG("rsx", "frame %u: %d draws", g_frame, g_draws_this_frame);
    g_draws_this_frame = 0;
    uint32_t ea = local_ea(offset);
    ID3D11ShaderResourceView *src = nullptr;
    float uv[4] = {1, 1, 0, 0};
    auto it = g_surfaces.find(ea);
    if (it != g_surfaces.end() && !it->second.depth) {
        src = it->second.srv;
        uv[0] = (float)width / it->second.w, uv[1] = (float)height / it->second.h;
    } else if (width && height) {
        // Nothing rendered there by the GPU: show memory contents (CPU-drawn).
        if (!g_cpu_tex) {
            D3D11_TEXTURE2D_DESC d{};
            d.Width = width, d.Height = height, d.MipLevels = 1, d.ArraySize = 1;
            d.Format = DXGI_FORMAT_R8G8B8A8_UNORM, d.SampleDesc.Count = 1;
            d.Usage = D3D11_USAGE_DEFAULT, d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            g_dev->CreateTexture2D(&d, nullptr, &g_cpu_tex);
            g_dev->CreateShaderResourceView(g_cpu_tex, nullptr, &g_cpu_srv);
        }
        std::vector<uint8_t> px((size_t)width * height * 4);
        const uint8_t *m = (const uint8_t *)mem::ptr(ea);
        for (uint32_t y = 0; y < height; y++)
            for (uint32_t x = 0; x < width; x++) {
                const uint8_t *p = m + (size_t)y * pitch + x * 4;
                uint8_t *d = &px[((size_t)y * width + x) * 4];
                d[0] = p[1], d[1] = p[2], d[2] = p[3], d[3] = 255;
            }
        g_ctx->UpdateSubresource(g_cpu_tex, 0, nullptr, px.data(), width * 4, 0);
        src = g_cpu_srv;
    }
    ID3D11ShaderResourceView *none[16] = {};
    g_ctx->PSSetShaderResources(0, 16, none);
    g_ctx->OMSetRenderTargets(1, &g_backbuffer, nullptr);
    // keep the swapchain the size of the window's client area
    RECT cr;
    GetClientRect(g_hwnd, &cr);
    static UINT bw = 1280, bh = 720;
    UINT cw = std::max<LONG>(cr.right, 1), ch = std::max<LONG>(cr.bottom, 1);
    if (cw != bw || ch != bh) {
        g_ctx->OMSetRenderTargets(0, nullptr, nullptr);
        release(g_backbuffer);
        g_swap->ResizeBuffers(0, cw, ch, DXGI_FORMAT_UNKNOWN, 0);
        ID3D11Texture2D *bb = nullptr;
        g_swap->GetBuffer(0, __uuidof(ID3D11Texture2D), (void **)&bb);
        g_dev->CreateRenderTargetView(bb, nullptr, &g_backbuffer);
        bb->Release();
        g_ctx->OMSetRenderTargets(1, &g_backbuffer, nullptr);
        bw = cw, bh = ch;
    }
    // letterbox to 16:9
    float vw = (float)cw, vh = (float)ch;
    if (vw / vh > 16.f / 9.f) vw = vh * 16.f / 9.f; else vh = vw * 9.f / 16.f;
    float black[4] = {0, 0, 0, 1};
    g_ctx->ClearRenderTargetView(g_backbuffer, black);
    D3D11_VIEWPORT vp{(cw - vw) / 2, (ch - vh) / 2, vw, vh, 0, 1};
    g_ctx->RSSetViewports(1, &vp);
    g_ctx->RSSetState(nullptr);
    g_ctx->OMSetBlendState(nullptr, nullptr, 0xffffffff);
    g_ctx->OMSetDepthStencilState(nullptr, 0);
    upload_cb(g_cb_present, uv, 16);
    g_ctx->VSSetShader(g_present_vs, nullptr, 0);
    g_ctx->PSSetShader(g_present_ps, nullptr, 0);
    g_ctx->PSSetConstantBuffers(0, 1, &g_cb_present);
    g_ctx->VSSetConstantBuffers(0, 1, &g_cb_present);
    g_ctx->PSSetShaderResources(0, 1, &src);
    g_ctx->PSSetSamplers(0, 1, &g_present_smp);
    g_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g_ctx->Draw(3, 0);
    g_swap->Present(0, 0);
}

void copy_m2mf() {
    // NV0039: line-by-line copy between local and main memory
    uint32_t in = ea_for(dma_location(reg(0x2184)), reg(0x230c));
    uint32_t out = ea_for(dma_location(reg(0x2188)), reg(0x2310));
    int32_t pin = (int32_t)reg(0x2314), pout = (int32_t)reg(0x2318);
    uint32_t len = reg(0x231c), lines = reg(0x2320);
    for (uint32_t l = 0; l < lines; l++)
        memmove(mem::ptr(out + l * pout), mem::ptr(in + l * pin), len);
}

void method(uint32_t m, uint32_t arg) {
    regs[m >> 2] = arg;
    if (m >= 0x0b80 && m < 0x0c00) { vp_ucode[g_vp_load++ & 2047] = arg; return; }
    if (m >= 0x1f00 && m < 0x1f80) { memcpy(&vp_consts[g_vc_load++ & 2047], &arg, 4); vp_consts_dirty = true; return; }
    if (m >= 0x1c00 && m < 0x1d00) { uint32_t i = (m - 0x1c00) >> 2; memcpy(&g_vattr[i / 4][i % 4], &arg, 4); return; }
    if (m >= 0x1880 && m < 0x1900) {  // SET_VERTEX_DATA2F_M
        uint32_t i = (m - 0x1880) >> 2;
        memcpy(&g_vattr[i / 2][i % 2], &arg, 4);
        g_vattr[i / 2][2] = 0, g_vattr[i / 2][3] = 1;
        return;
    }
    if (m >= 0x1940 && m < 0x1980) {  // SET_VERTEX_DATA4UB_M
        float *v = g_vattr[(m - 0x1940) >> 2];
        v[0] = (arg & 255) / 255.f, v[1] = ((arg >> 8) & 255) / 255.f, v[2] = ((arg >> 16) & 255) / 255.f, v[3] = (arg >> 24) / 255.f;
        return;
    }
    switch (m) {
    case 0x1e9c: g_vp_load = (arg & 511) * 4; break;
    case 0x1efc: g_vc_load = (arg & 511) * 4; break;
    case 0x1808: begin_end(arg); break;
    case 0x1814: draw_arrays(arg & 0xffffff, (arg >> 24) + 1); break;
    case 0x1824: draw_indexed(arg & 0xffffff, (arg >> 24) + 1); break;
    case 0x1818: {
        static int n;
        if (n++ < 3) LOG("rsx", "inline vertex arrays are not supported yet");
        break;
    }
    case 0x1d94: clear(arg); break;
    case 0x2328: copy_m2mf(); break;
    default:
        if (m >= 0x4000 && m != 0xfeac) {
            static bool seen[0x4000];
            if (!seen[m >> 2]) { seen[m >> 2] = true; LOG("rsx", "unhandled 2D-engine method 0x%04x = 0x%08x", m, arg); }
        }
    }
}
} // namespace rsx
