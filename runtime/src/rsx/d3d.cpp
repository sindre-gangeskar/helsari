// RSX -> Direct3D 11 backend. Runs entirely on the FIFO thread.
//
//  * Render targets are D3D textures keyed by their RSX address. Sampling a
//    texture at a render target's address uses the D3D copy directly.
//  * Vertex attributes are converted on the CPU into 16 float4 per vertex and
//    read in the vertex shader through SV_VertexID.
//  * Shaders are translated from RSX microcode and cached by microcode hash.
#include "rsx.h"

#include <algorithm>
#include <atomic>
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
// Multisampled surfaces: the RSX stores the samples of a pixel side by side
// (2x: 2 across; 4x: 2x2), so a 1280x720 2x target occupies 2560x720 and
// later passes read it that way. We render such surfaces supersampled at
// their memory size; the transform still uses the clip size (the viewport
// stretches it) and WPOS is mapped back to pixel space.
uint32_t g_aa_sx = 1, g_aa_sy = 1;
// Frame capture: draws of g_log_frame are logged, surfaces dumped when g_dump_frame begins.
std::atomic<bool> g_capture_req{false};
int g_log_frame = getenv("KZ2_LOG_DRAWS") ? atoi(getenv("KZ2_LOG_DRAWS")) : -1;
int g_dump_frame = getenv("KZ2_DUMP_RT") ? atoi(getenv("KZ2_DUMP_RT")) : -1;
int g_step_dumps;   // per-draw dumps taken in the current capture

/* ---------------- shaders ---------------- */
ID3D11VertexShader *g_present_vs;
ID3D11PixelShader *g_present_ps;
ID3D11PixelShader *g_depth_color_ps;    // depth -> RSX D24S8 bytes as A8R8G8B8
ID3D11PixelShader *g_reinterpret_ps;    // A8R8G8B8 texels -> another 32-bit texture format
ID3D11Buffer *g_cb_pass;                // mode constant for the conversion passes
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
    uint32_t pitch = 0;                         // bytes per row in guest memory
    bool dirty = false;                         // rendered to since last written back
    ID3D11Texture2D *staging = nullptr;         // CPU-readable copy for write-back
    // Depth surface read as an A8R8G8B8 texture (the D24S8 bytes), see depth_as_color().
    ID3D11Texture2D *as_color = nullptr;
    ID3D11RenderTargetView *as_color_rtv = nullptr;
    ID3D11ShaderResourceView *as_color_srv = nullptr;
    // Colour surface read through a texture format with another layout, see reinterpret_view().
    uint32_t view_fmt = 0;
    ID3D11Texture2D *view = nullptr;
    ID3D11RenderTargetView *view_rtv = nullptr;
    ID3D11ShaderResourceView *view_srv = nullptr;
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
    release(s.rtv); release(s.dsv); release(s.srv); release(s.tex); release(s.copy_srv); release(s.copy); release(s.staging);
    release(s.as_color_srv); release(s.as_color_rtv); release(s.as_color);
    release(s.view_srv); release(s.view_rtv); release(s.view);
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
    uint32_t aa = (fmt >> 12) & 0xf;                 // 3: diagonal 2x, 4/5: square 4x
    g_aa_sx = aa >= 3 && aa <= 5 ? 2 : 1;
    g_aa_sy = aa == 4 || aa == 5 ? 2 : 1;
    uint32_t sw = b.w * g_aa_sx, shh = b.h * g_aa_sy;
    uint32_t target = reg(0x0220);
    static const uint32_t off_m[4] = {0x0210, 0x0218, 0x0288, 0x028c};     // COLOR_A..D_OFFSET
    static const uint32_t pitch_m[4] = {0x020c, 0x021c, 0x0280, 0x0284};   // SURFACE_PITCH_A..D
    static const uint32_t dma_m[4] = {0x0194, 0x018c, 0x01b4, 0x01b8};     // CONTEXT_DMA_COLOR_A..D
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
        b.color[i] = get_surface(ea, cfmt, sw, shh, false);
        if (b.color[i]) rtv[i] = b.color[i]->rtv, b.color[i]->pitch = reg(pitch_m[i]);
    }
    ID3D11DepthStencilView *dsv = nullptr;
    if (zfmt) {
        uint32_t ea = ea_for(dma_location(reg(0x0198)), reg(0x0214));          // CONTEXT_DMA_ZETA
        b.depth = get_surface(ea, zfmt, sw, shh, true);
        if (b.depth) dsv = b.depth->dsv, b.depth->pitch = reg(0x022c);           // SURFACE_PITCH_Z
    }
    // A bound surface must not also be bound as a texture.
    ID3D11ShaderResourceView *none[16] = {};
    g_ctx->PSSetShaderResources(0, 16, none);
    g_ctx->OMSetRenderTargets(4, rtv, dsv);
    D3D11_VIEWPORT vp{0, 0, (float)sw, (float)shh, 0, 1};
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
    // One or two components per big-endian word: swap each word into a
    // matching D3D format (Y16_X16: Y in the high half, so X lands in R).
    if (base == 0x94 || base == 0x95 || base == 0x9c || base == 0x9f) {
        uint32_t bpp = base == 0x94 ? 2 : 4;
        if (!pitch || !linear) pitch = w * bpp;
        uint32_t lw = log2u(w), lh = log2u(h);
        const uint8_t *src = (const uint8_t *)mem::ptr(ea);
        row = w * bpp;
        out.resize((size_t)row * h);
        for (uint32_t y = 0; y < h; y++)
            for (uint32_t x = 0; x < w; x++) {
                size_t o = linear ? (size_t)y * pitch + (size_t)x * bpp : (size_t)swizzled_offset(x, y, lw, lh) * bpp;
                for (uint32_t k = 0; k < bpp; k++) out[(size_t)y * row + x * bpp + k] = src[o + bpp - 1 - k];
            }
        return base == 0x94 ? DXGI_FORMAT_R16_UNORM : base == 0x95 ? DXGI_FORMAT_R16G16_UNORM
             : base == 0x9c ? DXGI_FORMAT_R32_FLOAT : DXGI_FORMAT_R16G16_FLOAT;
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

// Copy a colour surface rendered since it was last written back to guest
// memory, in RSX layout (see write_back).
void write_back_surface(uint32_t ea, Surface &s) {
    if (!s.dirty || !s.pitch) return;
    s.dirty = false;
    uint32_t bpp = s.depth ? (s.fmt == 1 ? 2 : 4)
                           : s.fmt == 0xb ? 8 : s.fmt == 0xc ? 16 : s.fmt == 0x9 ? 1 : s.fmt == 0xa ? 2 : 4;
    uint32_t w = std::min(s.w, s.pitch / bpp), h = s.h;
    if (!w) return;
    if (!s.staging) {
        D3D11_TEXTURE2D_DESC d;
        s.tex->GetDesc(&d);
        d.Usage = D3D11_USAGE_STAGING, d.BindFlags = 0, d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (FAILED(g_dev->CreateTexture2D(&d, nullptr, &s.staging))) return;
    }
    g_ctx->CopyResource(s.staging, s.tex);
    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(g_ctx->Map(s.staging, 0, D3D11_MAP_READ, 0, &m))) return;
    for (uint32_t y = 0; y < h; y++) {
        const uint8_t *src = (const uint8_t *)m.pData + (size_t)y * m.RowPitch;
        uint8_t *dst = (uint8_t *)mem::ptr(ea + y * s.pitch);
        if (s.depth) {
            if (s.fmt == 1) {                                                       // Z16: big-endian
                for (uint32_t x = 0; x < w; x++) dst[2 * x] = src[2 * x + 1], dst[2 * x + 1] = src[2 * x];
            } else {                                                                // D24S8: BE (depth << 8 | stencil)
                for (uint32_t x = 0; x < w; x++) {
                    uint32_t v;
                    memcpy(&v, src + 4 * x, 4);
                    v = __builtin_bswap32((v & 0xffffff) << 8 | v >> 24);
                    memcpy(dst + 4 * x, &v, 4);
                }
            }
            continue;
        }
        switch (s.fmt) {
        case 0x9: memcpy(dst, src, w); break;                                   // B8
        case 0xa: for (uint32_t x = 0; x < w; x++) dst[2 * x] = src[2 * x + 1], dst[2 * x + 1] = src[2 * x]; break;
        case 0xb: case 0xc: case 0xd: {                                         // big-endian floats / halves
            uint32_t n = bpp == 8 ? 2 : 4;
            for (uint32_t k = 0; k < w * bpp; k += n)
                for (uint32_t j = 0; j < n; j++) dst[k + j] = src[k + n - 1 - j];
            break;
        }
        default:                                                                // A8R8G8B8 / X8R8G8B8: bytes A R G B
            for (uint32_t x = 0; x < w; x++)
                dst[4 * x] = src[4 * x + 3], dst[4 * x + 1] = src[4 * x], dst[4 * x + 2] = src[4 * x + 1],
                dst[4 * x + 3] = src[4 * x + 2];
        }
    }
    g_ctx->Unmap(s.staging, 0);
    static int logs;
    if (logs++ < 10) LOG("rsx", "write-back surface 0x%x fmt %u %ux%u pitch 0x%x", ea, s.fmt, w, h, s.pitch);
}

// Full-screen conversion pass from src into dst (w x h) with pixel shader ps
// and a mode constant. Runs while a draw is being set up, so the bound
// targets and viewport are restored (everything else is set again later).
void run_pass(ID3D11ShaderResourceView *src, ID3D11RenderTargetView *dst, uint32_t w, uint32_t h,
              ID3D11PixelShader *ps, uint32_t mode = 0) {
    ID3D11RenderTargetView *rtv[4];
    ID3D11DepthStencilView *dsv;
    g_ctx->OMGetRenderTargets(4, rtv, &dsv);
    UINT nvp = 1;
    D3D11_VIEWPORT vp;
    g_ctx->RSGetViewports(&nvp, &vp);

    ID3D11ShaderResourceView *none[16] = {};
    g_ctx->PSSetShaderResources(0, 16, none);
    g_ctx->OMSetRenderTargets(1, &dst, nullptr);
    D3D11_VIEWPORT cv{0, 0, (float)w, (float)h, 0, 1};
    g_ctx->RSSetViewports(1, &cv);
    g_ctx->RSSetState(nullptr);
    g_ctx->OMSetBlendState(nullptr, nullptr, 0xffffffff);
    g_ctx->OMSetDepthStencilState(nullptr, 0);
    g_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g_ctx->VSSetShader(g_present_vs, nullptr, 0);
    g_ctx->PSSetShader(ps, nullptr, 0);
    uint32_t m[4] = {mode, 0, 0, 0};
    upload_cb(g_cb_pass, m, sizeof m);
    g_ctx->PSSetConstantBuffers(0, 1, &g_cb_pass);
    g_ctx->PSSetShaderResources(0, 1, &src);
    g_ctx->Draw(3, 0);
    g_ctx->PSSetShaderResources(0, 16, none);

    g_ctx->OMSetRenderTargets(4, rtv, dsv);
    if (nvp) g_ctx->RSSetViewports(1, &vp);
    for (auto *r : rtv) release(r);
    release(dsv);
}

// A render target texture of w x h in format f, with views.
bool make_target(uint32_t w, uint32_t h, DXGI_FORMAT f, ID3D11Texture2D *&tex, ID3D11RenderTargetView *&rtv,
                 ID3D11ShaderResourceView *&srv) {
    D3D11_TEXTURE2D_DESC d{};
    d.Width = w, d.Height = h, d.MipLevels = 1, d.ArraySize = 1, d.SampleDesc.Count = 1;
    d.Format = f;
    d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(g_dev->CreateTexture2D(&d, nullptr, &tex))) return false;
    g_dev->CreateRenderTargetView(tex, nullptr, &rtv);
    g_dev->CreateShaderResourceView(tex, nullptr, &srv);
    return true;
}

// Games read the depth buffer through an A8R8G8B8 texture and rebuild the
// 24-bit depth from its bytes. In memory a D24S8 word is depth[23:16],
// depth[15:8], depth[7:0], stencil, i.e. A, R, G, B. Render that encoding
// into a colour texture (stencil reads as 0).
ID3D11ShaderResourceView *depth_as_color(Surface &s) {
    if (!s.as_color && !make_target(s.w, s.h, DXGI_FORMAT_R8G8B8A8_UNORM, s.as_color, s.as_color_rtv, s.as_color_srv))
        return nullptr;
    run_pass(s.srv, s.as_color_rtv, s.w, s.h, g_depth_color_ps);
    return s.as_color_srv;
}

// A 32-bit colour surface read through a texture format with another
// layout: rebuild each pixel's RSX word (A<<24 | R<<16 | G<<8 | B) and decode
// it as the texture format would. Killzone 2 packs two half floats per pixel
// into an A8R8G8B8 G-buffer target and reads them back as Y16_X16_FLOAT.
bool reinterpretable(uint32_t base) { return base == 0x9f || base == 0x95 || base == 0x9c; }
ID3D11ShaderResourceView *reinterpret_view(Surface &s, uint32_t base) {
    if (s.view_fmt != base) {
        release(s.view_srv), release(s.view_rtv), release(s.view);
        s.view_fmt = 0;
        if (!make_target(s.w, s.h, DXGI_FORMAT_R32G32B32A32_FLOAT, s.view, s.view_rtv, s.view_srv)) return nullptr;
        s.view_fmt = base;
    }
    run_pass(s.srv, s.view_rtv, s.w, s.h, g_reinterpret_ps, base);
    return s.view_srv;
}

// Does a surface of format sfmt hold texels exactly as texture format base reads them?
bool same_layout(uint32_t sfmt, uint32_t base) {
    switch (sfmt) {
    case 0x9: return base == 0x81;                                  // B8
    case 0xa: return base == 0x8b;                                  // G8B8
    case 0xb: return base == 0x9a;                                  // RGBA16F
    case 0xc: return base == 0x9b;                                  // RGBA32F
    case 0xd: return base == 0x9c;                                  // X32F
    default: return base == 0x85 || base == 0x9e;                   // 32-bit ARGB
    }
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
    // Render-to-texture: use the surface directly. Depth formats (0x90-0x93)
    // read the depth surface; colour formats where only a depth surface
    // exists read its bytes as colour.
    if (!cube) {
        uint32_t base = fmt & ~0x60u;
        bool depth_fmt = base >= 0x90 && base <= 0x93;
        auto ds = g_depth_surfaces.find(ea);
        bool ds_fits = ds != g_depth_surfaces.end() && ds->second.w == w && ds->second.h == h;
        if (depth_fmt && ds_fits) {
            if (reg(b + 8) >> 28) dim = (TexDim)(dim | TEX_SHADOW);           // compare function set
            return ds->second.srv;
        }
        // The same memory is often reused for buffers of another size (a shadow
        // map, later a 640x360 post-processing target): only a surface of the
        // texture's size can stand in for it; otherwise read guest memory,
        // where the surface was written back.
        auto s = g_surfaces.find(ea);
        if (s != g_surfaces.end()) {
            Surface &rt = s->second;
            if (rt.w == w && rt.h == h) {
                if (same_layout(rt.fmt, base)) return rt.srv;
                if (color_format(rt.fmt) == DXGI_FORMAT_R8G8B8A8_UNORM && reinterpretable(base))
                    return reinterpret_view(rt, base);
            }
            write_back_surface(ea, rt);
        }
        if (ds_fits && base == 0x85) return depth_as_color(ds->second);    // A8R8G8B8 view of D24S8
        if (ds != g_depth_surfaces.end()) write_back_surface(ea, ds->second);
    }

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
        // Diagnostic only (KZ2_DEBUG_IDENTITY_LUT): an all-zero 64x64 A8R8G8B8 texture
        // (the colour-grading table) becomes an identity ramp, to see what else renders.
        static const bool identity_lut = getenv("KZ2_DEBUG_IDENTITY_LUT") != nullptr;
        if (identity_lut && w == 64 && h == 64 && (fmt & ~0x60u) == 0x85 &&
            std::all_of(data[f].begin(), data[f].end(), [](uint8_t b) { return b == 0; })) {
            for (uint32_t y = 0; y < h; y++)
                for (uint32_t x = 0; x < w; x++) {
                    uint8_t v = (uint8_t)(x * 255 / 63), *d = &data[f][(size_t)y * row + x * 4];
                    d[0] = v, d[1] = v, d[2] = 0, d[3] = v;     // with remap aa93: rgb = v, a (desaturate) = 0
                }
        }
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
// shadow: depth texture compared against the lookup z with the address register's zfunc.
ID3D11SamplerState *get_sampler(int unit, bool shadow) {
    uint32_t b = 0x1a00 + unit * 32;
    uint32_t addr = reg(b + 8), filt = reg(b + 0x14);
    uint64_t key = ((uint64_t)addr << 32) | (filt & 0xffff0000) | shadow;
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
    if (shadow) d.Filter = lin ? D3D11_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT : D3D11_FILTER_COMPARISON_MIN_MAG_MIP_POINT;
    d.AddressU = wrap(addr & 0xf);
    d.AddressV = wrap((addr >> 8) & 0xf);
    d.AddressW = wrap((addr >> 16) & 0xf);
    d.MaxLOD = D3D11_FLOAT32_MAX;
    // RSX zfunc 0..7 (never, less, equal, lequal, greater, notequal, gequal, always) = D3D11 func - 1
    d.ComparisonFunc = shadow ? (D3D11_COMPARISON_FUNC)((addr >> 28) + 1) : D3D11_COMPARISON_NEVER;
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
    r.left *= g_aa_sx, r.right *= g_aa_sx, r.top *= g_aa_sy, r.bottom *= g_aa_sy;
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
uint64_t g_vs_hash;   // of the program used by the current draw (frame capture log)
ID3D11VertexShader *vertex_shader() {
    uint32_t start = reg(0x1ea0) & 511;
    uint64_t h = 1469598103934665603ull;
    for (uint32_t i = start; i < 512; i++) {
        h = fnv(&vp_ucode[i * 4], 16, h);
        if (vp_ucode[i * 4 + 3] & 1) break;
    }
    g_vs_hash = h;
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

PsEntry *pixel_shader(uint32_t fp_ea, const uint8_t texdim[16], const uint16_t remap[16]) {
    FpScan sc = scan_fp(fp_ea);
    uint32_t control = reg(0x1d60);
    uint32_t alpha = (reg(0x0304) & 1) ? reg(0x0308) : 0;
    uint64_t h = fnv(remap, 32, fnv(texdim, 16, fnv(&control, 4, fnv(&alpha, 4, sc.hash))));
    auto it = g_ps.find(h);
    if (it != g_ps.end()) return &it->second;
    FpKey key{fp_ea, control, {}, alpha, {}};
    memcpy(key.texdim, texdim, 16);
    memcpy(key.remap, remap, 32);
    FpInfo fi = translate_fp(key);
    PsEntry e{nullptr, sc.consts};
    if (ID3DBlob *b = compile(fi.hlsl, "ps_5_0", h)) {
        g_dev->CreatePixelShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &e.ps);
        b->Release();
    }
    return &(g_ps[h] = e);
}

// Debug: write a RGBA8 / RGBA16F colour surface to <name>_f<fmt>.bmp (half floats clamped to 0..1).
void dump_surface(Surface &s, const char *name) {
    DXGI_FORMAT df = color_format(s.fmt);
    bool half = df == DXGI_FORMAT_R16G16B16A16_FLOAT;
    if (df != DXGI_FORMAT_R8G8B8A8_UNORM && !half) return;
    D3D11_TEXTURE2D_DESC d;
    s.tex->GetDesc(&d);
    d.Usage = D3D11_USAGE_STAGING, d.BindFlags = 0, d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Texture2D *st = nullptr;
    g_dev->CreateTexture2D(&d, nullptr, &st);
    g_ctx->CopyResource(st, s.tex);
    D3D11_MAPPED_SUBRESOURCE m;
    g_ctx->Map(st, 0, D3D11_MAP_READ, 0, &m);
    char path[64];
    snprintf(path, sizeof path, "%s_f%x.bmp", name, s.fmt);
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
            for (UINT x = 0; x < d.Width; x++) {
                uint8_t c[3];
                for (int k = 0; k < 3; k++) {
                    if (!half) { c[k] = p[x * 4 + k]; continue; }
                    uint16_t hv;
                    memcpy(&hv, p + x * 8 + 2 * k, 2);
                    c[k] = (uint8_t)(std::clamp(h2f(hv), 0.f, 1.f) * 255.f + .5f);
                }
                row[x * 4] = c[2], row[x * 4 + 1] = c[1], row[x * 4 + 2] = c[0], row[x * 4 + 3] = 255;
            }
            fwrite(row.data(), 1, row.size(), f);
        }
        fclose(f);
    }
    g_ctx->Unmap(st, 0);
    st->Release();
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
        smp[t] = get_sampler(t, texdim[t] & TEX_SHADOW);
    }
    uint16_t remap[16];
    for (int t = 0; t < 16; t++) remap[t] = (reg(0x1a0c + t * 32) & 0x80000000) ? reg(0x1a10 + t * 32) & 0xffff : 0xaae4;
    PsEntry *ps = pixel_shader(fp_ea, texdim, remap);
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
    px.wpos[2] = (float)g_aa_sx, px.wpos[3] = (float)g_aa_sy;
    for (size_t k = 0; k < ps->consts.size(); k++)
        for (int c = 0; c < 4; c++) px.fc[k * 4 + c] = fp_const_word(fp_ea + ps->consts[k] + 4 * c);
    upload_cb(g_cb_px, &px, 32 + 256 + ps->consts.size() * 16);
    if ((int)g_frame == g_log_frame) {
        float f[8];
        memcpy(f, px.fc, 32);
        uint64_t psh = 0;
        for (auto &[k, v] : g_ps) if (&v == ps) psh = k;
        LOG("draw", "#%d rt=%x tex0=%x ps=%016llx cmask=%08x blend=%x/%x/%x fc0=(%g %g %g %g) fc1=(%g %g %g %g) n=%zu",
            g_draws_this_frame, b.color[0] ? b.color[0]->ea : 0, srv[0] ? ea_for((reg(0x1a04) & 3) == 2, reg(0x1a00)) : 0,
            (unsigned long long)psh, reg(0x324), reg(0x310), reg(0x314), reg(0x318), f[0], f[1], f[2], f[3], f[4], f[5], f[6], f[7], list.size());
        LOG("draw", "   depth test %u func %x write %u, stencil %u func %x ref %x mask %x/%x ops %x/%x/%x two-sided %u, "
            "depth bounds %u [%g, %g]", reg(0x0a74) & 1, reg(0x0a6c), reg(0x0a70) & 1, reg(0x0328) & 1, reg(0x0330),
            reg(0x0334), reg(0x0338), reg(0x032c), reg(0x033c), reg(0x0340), reg(0x0344), reg(0x0348) & 1,
            reg(0x0380) & 1, regf(0x0384), regf(0x0388));
        LOG("draw", "   surface fmt=%08x %ux%u target=%x color=%x/%x/%x/%x depth=%x pitch=%x", reg(0x0208), b.w, b.h,
            reg(0x0220), b.color[0] ? b.color[0]->ea : 0, b.color[1] ? b.color[1]->ea : 0, b.color[2] ? b.color[2]->ea : 0,
            b.color[3] ? b.color[3]->ea : 0, b.depth ? b.depth->ea : 0, reg(0x020c));
        for (int t = 0; t < 16; t++)
            if (reg(0x1a0c + t * 32) & 0x80000000) {
                uint32_t tea = ea_for((reg(0x1a04 + t * 32) & 3) == 2, reg(0x1a00 + t * 32));
                auto rt = g_surfaces.find(tea);
                LOG("draw", "   tex%d ea=%x fmt=%08x rect=%08x pitch=%x filter=%08x addr=%08x remap=%04x %s", t, tea,
                    reg(0x1a04 + t * 32), reg(0x1a18 + t * 32), reg(0x1840 + t * 4) & 0xfffff, reg(0x1a14 + t * 32),
                    reg(0x1a08 + t * 32), reg(0x1a10 + t * 32) & 0xffff,
                    rt != g_surfaces.end() ? ("(render target fmt " + std::to_string(rt->second.fmt) + " " +
                                              std::to_string(rt->second.w) + "x" + std::to_string(rt->second.h) + ")").c_str()
                                           : "");
            }
        // Vertex shader and the first vertices' attributes: every full-screen pass, or KZ2_LOG_ATTRS=<draw>.
        static int attr_draw = getenv("KZ2_LOG_ATTRS") ? atoi(getenv("KZ2_LOG_ATTRS")) : -1;
        if (g_draws_this_frame == attr_draw || list.size() <= 6) {
            LOG("draw", "   vs=%016llx inputs=%04x outputs=%08x", (unsigned long long)g_vs_hash, reg(0x1ff0), reg(0x1ff4));
            for (int k = 464; k < 468; k++)
                LOG("draw", "   c[%d] = %g %g %g %g", k, vp_consts[k * 4], vp_consts[k * 4 + 1], vp_consts[k * 4 + 2],
                    vp_consts[k * 4 + 3]);
            LOG("draw", "   viewport scale %g %g %g offset %g %g %g, clip %08x %08x, origin %08x", regf(0x0a30),
                regf(0x0a34), regf(0x0a38), regf(0x0a20), regf(0x0a24), regf(0x0a28), reg(0x0200), reg(0x0204),
                reg(0x1d88));
            for (uint32_t v = 0; v < 3; v++) {
                float vv[64];
                fetch_vertex(vv, vmin + v);
                for (int a = 0; a < 16; a++) if (reg(0x1ff0) & (1 << a))
                    LOG("draw", "   v%u attr%d fmt=%08x off=%08x: %g %g %g %g", v, a, reg(0x1740 + a * 4), reg(0x1680 + a * 4),
                        vv[a * 4], vv[a * 4 + 1], vv[a * 4 + 2], vv[a * 4 + 3]);
            }
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
    // Only what the RSX would write is dirty: a zeta buffer stays bound while depth
    // and stencil are off, and depth-only passes mask colour writes.
    if (reg(0x0324)) for (auto *c : b.color) if (c) c->dirty = true;
    bool zwrite = ((reg(0x0a74) & 1) && (reg(0x0a70) & 1)) || (reg(0x0328) & 1);
    if (b.depth && zwrite) b.depth->dirty = true;
    // Capture: after each small draw (post-processing quads), save its target.
    // (Square targets are shadow maps: skipped.)
    if ((int)g_frame == g_log_frame && list.size() <= 96 && b.color[0] && b.color[0]->w != b.color[0]->h &&
        g_step_dumps < 80) {
        g_step_dumps++;
        char name[64];
        snprintf(name, sizeof name, "step_%04d_%08x", g_draws_this_frame, b.color[0]->ea);
        dump_surface(*b.color[0], name);
    }
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
    static const char REINTERPRET_HLSL[] =
        "Texture2D<float4> c : register(t0);\n"
        "cbuffer M : register(b0) { uint4 mode; };\n"
        "float4 ps(float4 p : SV_Position) : SV_Target {\n"
        "  uint4 b = (uint4)round(saturate(c.Load(int3(p.xy, 0))) * 255);\n"
        "  uint w = b.a << 24 | b.r << 16 | b.g << 8 | b.b;\n"
        "  if (mode.x == 0x9f) return float4(f16tof32(w & 0xffff), f16tof32(w >> 16), 0, 1);\n"
        "  if (mode.x == 0x95) return float4((w & 0xffff) / 65535.0, (w >> 16) / 65535.0, 0, 1);\n"
        "  if (mode.x == 0x9c) return asfloat(w).xxxx;\n"
        "  return float4(b) / 255.0; }\n";
    if (FAILED(D3DCompile(REINTERPRET_HLSL, sizeof REINTERPRET_HLSL - 1, nullptr, nullptr, nullptr, "ps", "ps_5_0", 0, 0,
                          &code, &err))) {
        LOG("rsx", "reinterpret shader: %s", err ? (const char *)err->GetBufferPointer() : "?");
    } else {
        g_dev->CreatePixelShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &g_reinterpret_ps);
        code->Release();
    }
    g_cb_pass = make_cb(16);
    static const char DEPTH_COLOR_HLSL[] =
        "Texture2D<float> d : register(t0);\n"
        "float4 ps(float4 p : SV_Position) : SV_Target {\n"
        "  uint z = (uint)round(d.Load(int3(p.xy, 0)) * 16777215.0);\n"
        "  return float4(((z >> 8) & 255) / 255.0, (z & 255) / 255.0, 0, (z >> 16) / 255.0); }\n";
    if (FAILED(D3DCompile(DEPTH_COLOR_HLSL, sizeof DEPTH_COLOR_HLSL - 1, nullptr, nullptr, nullptr, "ps", "ps_5_0", 0, 0,
                          &code, &err))) {
        LOG("rsx", "depth->colour shader: %s", err ? (const char *)err->GetBufferPointer() : "?");
    } else {
        g_dev->CreatePixelShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &g_depth_color_ps);
        code->Release();
    }
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
        for (auto *s : b.color) if (s) g_ctx->ClearRenderTargetView(s->rtv, f), s->dirty = true;
    }
    if ((mask & 3) && b.depth) {
        uint32_t z = reg(0x1d8c);
        UINT fl = (mask & 1 ? D3D11_CLEAR_DEPTH : 0) | (mask & 2 ? D3D11_CLEAR_STENCIL : 0);
        float depth = b.depth->fmt == 1 ? (z & 0xffff) / 65535.f : (z >> 8) / 16777215.f;
        g_ctx->ClearDepthStencilView(b.depth->dsv, fl, depth, (UINT8)(z & 0xff));
        b.depth->dirty = true;
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

// Debug: write every colour surface (see dump_surface).
void dump_surfaces() {
    for (auto &[ea, s] : g_surfaces) {
        char name[32];
        snprintf(name, sizeof name, "rt_%08x", ea);
        dump_surface(s, name);
    }
    LOG("rsx", "dumped %zu surfaces at frame %u", g_surfaces.size(), g_frame);
}

void request_capture() { g_capture_req = true; }

// Copy colour surfaces rendered since the last sync point back to guest
// memory in RSX layout. Called when the RSX signals progress (labels,
// semaphores, user commands, flips): from then on the PPU and SPUs may read
// what it rendered - Killzone 2 post-processes downsampled frames on the SPUs.
void write_back() {
    for (auto &[ea, s] : g_surfaces) write_back_surface(ea, s);
    for (auto &[ea, s] : g_depth_surfaces) write_back_surface(ea, s);
}

// Debug: KZ2_SCREENSHOT=<sec> writes the presented frame, half size, to
// shot_<seconds since start>.png every that many seconds.
void write_png(const char *path, const uint8_t *rgb, uint32_t w, uint32_t h) {
    static uint32_t crc_table[256] = {};
    if (!crc_table[1])
        for (uint32_t n = 0; n < 256; n++) {
            uint32_t c = n;
            for (int k = 0; k < 8; k++) c = c & 1 ? 0xedb88320u ^ (c >> 1) : c >> 1;
            crc_table[n] = c;
        }
    std::vector<uint8_t> out = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
    auto be32 = [](std::vector<uint8_t> &v, uint32_t x) { for (int s = 24; s >= 0; s -= 8) v.push_back((uint8_t)(x >> s)); };
    auto chunk = [&](const char *type, const std::vector<uint8_t> &data) {
        be32(out, (uint32_t)data.size());
        size_t start = out.size();
        out.insert(out.end(), type, type + 4);
        out.insert(out.end(), data.begin(), data.end());
        uint32_t c = 0xffffffffu;
        for (size_t i = start; i < out.size(); i++) c = crc_table[(c ^ out[i]) & 0xff] ^ (c >> 8);
        be32(out, c ^ 0xffffffffu);
    };
    std::vector<uint8_t> ihdr;
    be32(ihdr, w), be32(ihdr, h);
    ihdr.insert(ihdr.end(), {8, 2, 0, 0, 0});              // 8-bit RGB
    chunk("IHDR", ihdr);
    // zlib stream of stored (uncompressed) deflate blocks
    std::vector<uint8_t> raw;
    for (uint32_t y = 0; y < h; y++) {
        raw.push_back(0);
        raw.insert(raw.end(), rgb + (size_t)y * w * 3, rgb + (size_t)(y + 1) * w * 3);
    }
    std::vector<uint8_t> z = {0x78, 0x01};
    for (size_t i = 0; i < raw.size(); i += 65535) {
        uint16_t n = (uint16_t)std::min<size_t>(65535, raw.size() - i);
        z.insert(z.end(), {(uint8_t)(i + n == raw.size()), (uint8_t)n, (uint8_t)(n >> 8), (uint8_t)~n, (uint8_t)(~n >> 8)});
        z.insert(z.end(), raw.begin() + i, raw.begin() + i + n);
    }
    uint32_t a = 1, b = 0;
    for (uint8_t c : raw) a = (a + c) % 65521, b = (b + a) % 65521;
    be32(z, b << 16 | a);
    chunk("IDAT", z);
    chunk("IEND", {});
    if (FILE *f = fopen(path, "wb")) { fwrite(out.data(), 1, out.size(), f); fclose(f); }
}

void maybe_screenshot() {
    static const double interval = getenv("KZ2_SCREENSHOT") ? atof(getenv("KZ2_SCREENSHOT")) : 0;
    static const ULONGLONG t0 = GetTickCount64();
    static double next = interval;
    if (interval <= 0) return;
    double now = (GetTickCount64() - t0) / 1000.0;
    if (now < next) return;
    next = now + interval;
    ID3D11Resource *bb = nullptr;
    g_backbuffer->GetResource(&bb);
    D3D11_TEXTURE2D_DESC d;
    ((ID3D11Texture2D *)bb)->GetDesc(&d);
    d.Usage = D3D11_USAGE_STAGING, d.BindFlags = 0, d.CPUAccessFlags = D3D11_CPU_ACCESS_READ, d.MiscFlags = 0;
    ID3D11Texture2D *st = nullptr;
    if (SUCCEEDED(g_dev->CreateTexture2D(&d, nullptr, &st))) {
        g_ctx->CopyResource(st, bb);
        D3D11_MAPPED_SUBRESOURCE m;
        if (SUCCEEDED(g_ctx->Map(st, 0, D3D11_MAP_READ, 0, &m))) {
            uint32_t w = d.Width / 2, h = d.Height / 2;
            std::vector<uint8_t> rgb((size_t)w * h * 3);
            for (uint32_t y = 0; y < h; y++)
                for (uint32_t x = 0; x < w; x++)
                    memcpy(&rgb[((size_t)y * w + x) * 3], (const uint8_t *)m.pData + (size_t)y * 2 * m.RowPitch + x * 8, 3);
            char path[64];
            snprintf(path, sizeof path, "shot_%04d.png", (int)now);
            write_png(path, rgb.data(), w, h);
            LOG("rsx", "screenshot %s (frame %u)", path, g_frame);
            g_ctx->Unmap(st, 0);
        }
        st->Release();
    }
    bb->Release();
}

void flip(uint32_t offset, uint32_t width, uint32_t height, uint32_t pitch) {
    if (!g_ok) return;
    g_frame++;
    if ((int)g_frame == g_dump_frame) dump_surfaces();
    if (g_capture_req.exchange(false)) {
        g_log_frame = (int)g_frame, g_dump_frame = (int)g_frame + 1, g_step_dumps = 0;
        LOG("rsx", "capture: logging draws of frame %u, dumping its surfaces at the end", g_frame);
    }
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
    maybe_screenshot();
    g_swap->Present(0, 0);
}

void write_back();
void copy_m2mf() {
    // NV0039: line-by-line copy between local and main memory. The source may
    // be a surface rendered earlier in the stream: write it back first.
    write_back();
    uint32_t in = ea_for(dma_location(reg(0x2184)), reg(0x230c));
    uint32_t out = ea_for(dma_location(reg(0x2188)), reg(0x2310));
    int32_t pin = (int32_t)reg(0x2314), pout = (int32_t)reg(0x2318);
    uint32_t len = reg(0x231c), lines = reg(0x2320);
    static int logs;
    if (logs++ < 40)
        LOG("rsx", "m2mf copy 0x%x -> 0x%x: %u lines of %u bytes (pitch in %d out %d)", in, out, lines, len, pin, pout);
    for (uint32_t l = 0; l < lines; l++)
        memmove(mem::ptr(out + l * pout), mem::ptr(in + l * pin), len);
}

void method(uint32_t m, uint32_t arg) {
    regs[m >> 2] = arg;
    // Sync points: whoever waits on these may read what was rendered.
    if (m == 0x006c || m == 0x1d70 || m == 0x1d74 || m == 0xeb00 || m == 0xfeac) write_back();
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
