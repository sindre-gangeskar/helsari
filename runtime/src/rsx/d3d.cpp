// RSX -> Direct3D 11 backend. Runs entirely on the FIFO thread.
//
//  * Render targets are D3D textures keyed by their RSX address. Sampling a
//    texture at a render target's address uses the D3D copy directly.
//  * Vertex attributes are converted on the CPU into 16 float4 per vertex and
//    read in the vertex shader through SV_VertexID.
//  * Shaders are translated from RSX microcode and cached by microcode hash.
#include "rsx.h"
#include "idle_backoff.h"

#include <algorithm>
#include <atomic>
#include <set>
#include <windows.h>
#include <d3d11_1.h>
#include <dxgi1_6.h>
#include <psapi.h>
#include <d3dcompiler.h>

#include <chrono>
#include <cmath>
#include <filesystem>
#include <unordered_map>

namespace rsx {
uint32_t regs[0x10000 / 4];
uint32_t vp_ucode[512 * 4];
float vp_consts[512 * 4];
bool vp_consts_dirty = true;
// Backend costs on the FIFO thread, reported with the fps (rsx::stats_line).
uint64_t g_wb_us, g_wb_calls, g_draw_us, g_draw_calls, g_present_us;
uint64_t g_hash_ns, g_hash_n;   // texture change detection (hashing guest memory)
// Draws dropped before reaching the GPU, by reason (fps log): no vertex ranges, no render
// target, index range too large, ring buffer full, no primitives, shader missing.
uint64_t g_skip[6];
uint64_t g_wbs_us, g_wbs_n, g_tex_us, g_tex_n, g_compile_us, g_compile_n;   // inside the above
uint64_t g_fetch_us, g_fetch_verts, g_fetch_used, g_dcall_us, g_map_us;      // vertex fetch (of which Map+copy)
struct ScopedUs {
    uint64_t &acc;
    std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
    ~ScopedUs() { acc += std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0).count(); }
};

uint64_t fnv(const void *p, size_t n, uint64_t h) {
    const uint8_t *b = (const uint8_t *)p;
    for (size_t i = 0; i < n; i++) h = (h ^ b[i]) * 1099511628211ull;
    return h;
}

// In-memory keys only (shader, state and texture lookups, computed per draw): eight bytes at a
// step. The disk shader cache names files with fnv, which stays as it is.
static inline uint64_t hfast(const void *p, size_t n, uint64_t h = 1469598103934665603ull) {
    const uint8_t *b = (const uint8_t *)p;
    for (; n >= 8; n -= 8, b += 8) {
        uint64_t w;
        memcpy(&w, b, 8);
        h = (h ^ w) * 0x9e3779b97f4a7c15ull, h ^= h >> 32;
    }
    for (; n; n--, b++) h = (h ^ *b) * 1099511628211ull;
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
// Scene draws per frame (draws into 4 colour targets at once: the G-buffer pass),
// min / sum / max over the fps log interval: the visible set should be stable.
int g_scene_draws, g_scene_min = 1 << 30, g_scene_max, g_scene_frames;
uint64_t g_scene_sum;
// Multisampled surfaces: the RSX stores the samples of a pixel side by side
// (2x: 2 across; 4x: 2x2), so a 1280x720 2x target occupies 2560x720 and
// later passes read it that way. We render such surfaces supersampled at
// their memory size; the transform still uses the clip size (the viewport
// stretches it) and WPOS is mapped back to pixel space.
uint32_t g_aa_sx = 1, g_aa_sy = 1;
// Frame capture: draws of g_log_frame are logged, surfaces dumped when g_dump_frame begins.
std::atomic<bool> g_capture_req{false};
std::atomic<bool> g_no_cube{false};   // F11 diagnostic: cube maps read as black
int g_log_frame = getenv("KZ2_LOG_DRAWS") ? atoi(getenv("KZ2_LOG_DRAWS")) : -1;
int g_dump_frame = getenv("KZ2_DUMP_RT") ? atoi(getenv("KZ2_DUMP_RT")) : -1;
int g_step_dumps;   // per-draw dumps taken in the current capture

/* ---------------- shaders ---------------- */
ID3D11VertexShader *g_present_vs;
ID3D11PixelShader *g_present_ps;
ID3D11PixelShader *g_depth_color_ps;    // depth -> RSX D24S8 bytes as A8R8G8B8
ID3D11PixelShader *g_depth_copy_ps;     // depth texture -> SV_Depth (proxy copies)
ID3D11DepthStencilState *g_ds_always;   // depth test always, write on
ID3D11PixelShader *g_reinterpret_ps;    // A8R8G8B8 texels -> another 32-bit texture format
ID3D11Buffer *g_cb_pass;                // mode constant for the conversion passes
ID3D11Buffer *g_cb_unit_uv;             // uvscale (1, 1) for g_present_vs in run_pass
std::unordered_map<uint64_t, ID3D11VertexShader *> g_vs;
std::unordered_map<uint64_t, std::string> g_vs_hlsl;   // translated source, for the frame capture
struct VsWin { uint16_t lo_base = 0, lo_count = 512, hi_base = 0, hi_count = 0; };
std::unordered_map<uint64_t, VsWin> g_vs_win;          // constant windows per shader (VpInfo)
VsWin g_cur_win;                                        // of the current draw's shader
std::unordered_map<uint64_t, bool> g_vs_dumped;
struct PsEntry { ID3D11PixelShader *ps; std::vector<uint32_t> consts; std::string hlsl; uint32_t control; bool dumped; };
std::unordered_map<uint64_t, PsEntry> g_ps;

ID3DBlob *compile(const std::string &src, const char *target, uint64_t hash) {
    ID3DBlob *code = nullptr, *err = nullptr;
    // Disk cache of compiled shaders (shader_cache/<target>_<source hash>.bin): later
    // runs load bytecode instead of compiling. KZ2_NO_SHADER_CACHE=1: always compile.
    static const bool no_cache = getenv("KZ2_NO_SHADER_CACHE") && atoi(getenv("KZ2_NO_SHADER_CACHE"));
    char cpath[96];
    snprintf(cpath, sizeof cpath, "shader_cache/%s_%016llx.bin", target,
             (unsigned long long)fnv(target, strlen(target), fnv(src.data(), src.size())));
    if (!no_cache)
        if (FILE *f = fopen(cpath, "rb")) {
            fseek(f, 0, SEEK_END);
            long n = ftell(f);
            fseek(f, 0, SEEK_SET);
            if (n > 0 && SUCCEEDED(D3DCreateBlob((SIZE_T)n, &code)) && fread(code->GetBufferPointer(), 1, n, f) == (size_t)n) {
                fclose(f);
                return code;
            }
            if (code) code->Release(), code = nullptr;
            fclose(f);
        }
    ScopedUs timer{g_compile_us};
    g_compile_n++;
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
    if (!no_cache) {
        std::error_code ec;
        std::filesystem::create_directories("shader_cache", ec);
        if (FILE *f = fopen(cpath, "wb")) { fwrite(code->GetBufferPointer(), 1, code->GetBufferSize(), f); fclose(f); }
    }
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

// Per-draw vertex and index data are appended to one large dynamic buffer
// with MAP_WRITE_NO_OVERWRITE and only discarded when it wraps; draws locate
// their data with BaseVertexLocation / StartIndexLocation. Discarding a buffer
// on every draw makes the driver run out of renames and wait for the GPU.
struct Ring : DynBuf { UINT pos = 0; };
Ring g_vring, g_iring;
bool g_ring_ok;   // MAP_WRITE_NO_OVERWRITE is allowed on the vertex SRV buffer

// Space for `bytes` (a multiple of `unit`): returns the mapped pointer and its offset in units.
void *ring_map(Ring &r, UINT bytes, UINT unit, UINT bind, bool structured, UINT *offset_units) {
    if (!g_ring_ok) { *offset_units = 0; return map_buf(r, bytes, bind, structured); }
    constexpr UINT RING_SIZE = 64 << 20;
    D3D11_MAP mode = D3D11_MAP_WRITE_NO_OVERWRITE;
    if (bytes > r.size || r.size < RING_SIZE) {               // (re)create, large enough
        UINT want = std::max(RING_SIZE, bytes * 2);
        void *p = map_buf(r, want, bind, structured);            // creates and maps with DISCARD
        g_ctx->Unmap(r.buf, 0);
        (void)p;
        r.pos = 0;
        mode = D3D11_MAP_WRITE_DISCARD;
    } else if (r.pos + bytes > r.size) {
        r.pos = 0;
        mode = D3D11_MAP_WRITE_DISCARD;
    }
    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(g_ctx->Map(r.buf, 0, mode, 0, &m))) { *offset_units = 0; return nullptr; }
    *offset_units = r.pos / unit;
    void *p = (uint8_t *)m.pData + r.pos;
    r.pos = (r.pos + bytes + unit - 1) / unit * unit;
    return p;
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

// Per-draw constants are appended to one large constant buffer and bound by
// offset (D3D11.1 VSSetConstantBuffers1). Discarding a constant buffer on
// every draw (~1700 per frame, 8 KB each) made the driver allocate fresh
// memory each time, which it kept: the process grew ~30 MB per frame.
struct CbSlice { UINT first = 0, num = 0, gen = ~0u; };
ID3D11DeviceContext1 *g_ctx1;
ID3D11Buffer *g_cbring;
UINT g_cbring_pos, g_cbring_gen;
constexpr UINT CBRING_SIZE = 16 << 20;
// The ring stays mapped across a draw's pushes (one Map/Unmap per draw instead of
// one per block); cb_flush() unmaps it before the draw binds it.
uint8_t *g_cb_map;
void cb_flush() {
    if (g_cb_map) g_ctx->Unmap(g_cbring, 0), g_cb_map = nullptr;
}
CbSlice cb_push(const void *data, size_t n) {
    UINT bytes = (UINT)((n + 255) & ~size_t(255));                    // offsets and sizes: multiples of 16 constants
    D3D11_MAP mode = D3D11_MAP_WRITE_NO_OVERWRITE;
    if (g_cbring_pos + bytes > CBRING_SIZE) {
        cb_flush();
        g_cbring_pos = 0, g_cbring_gen++, mode = D3D11_MAP_WRITE_DISCARD;
    }
    if (!g_cb_map) {
        D3D11_MAPPED_SUBRESOURCE m;
        if (FAILED(g_ctx->Map(g_cbring, 0, mode, 0, &m))) return {};
        g_cb_map = (uint8_t *)m.pData;
    }
    memcpy(g_cb_map + g_cbring_pos, data, n);
    CbSlice s{g_cbring_pos / 16, bytes / 16, g_cbring_gen};
    g_cbring_pos += bytes;
    return s;
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
    // A draw may bind targets of different sizes (RSX: memory with a pitch, the draw
    // covers the top-left of each); D3D11 binds none of them then. The draw renders
    // into a draw-sized stand-in instead, see bind_surfaces().
    struct Surface *proxy = nullptr;
    uint8_t newer = 0;                          // with a proxy: 1 this surface newer, 2 the proxy newer
    // Early readback: serial of the last change, and of the change the staging copy holds.
    uint64_t drawn_serial = 1, copied_serial = 0;
    // A texture reading the top-left w x h of this surface (same address and pitch):
    // a GPU copy of that region, refreshed when the surface changed (sub_view()).
    uint32_t sub_w = 0, sub_h = 0;
    uint64_t sub_serial = 0;
    ID3D11Texture2D *sub_tex = nullptr;
    ID3D11ShaderResourceView *sub_srv = nullptr;
    ID3D11RenderTargetView *sub_rtv = nullptr;
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

void free_surface(Surface &s) {
    release(s.rtv); release(s.dsv); release(s.srv); release(s.tex); release(s.copy_srv); release(s.copy); release(s.staging);
    release(s.as_color_srv); release(s.as_color_rtv); release(s.as_color);
    release(s.view_srv); release(s.view_rtv); release(s.view);
    release(s.sub_srv); release(s.sub_rtv); release(s.sub_tex);
    if (s.proxy) { free_surface(*s.proxy); delete s.proxy; }
    s = Surface{};
}

void create_surface_tex(Surface &s, uint32_t nw, uint32_t nh);

Surface *get_surface(uint32_t ea, uint32_t fmt, uint32_t w, uint32_t h, bool depth) {
    if (!w || !h) return nullptr;
    Surface &s = (depth ? g_depth_surfaces : g_surfaces)[ea];
    if (s.tex && s.fmt == fmt && s.depth == depth && s.w >= w && s.h >= h) { s.last_frame = g_frame; return &s; }
    uint32_t nw = std::max(w, s.fmt == fmt ? s.w : 0), nh = std::max(h, s.fmt == fmt ? s.h : 0);
    free_surface(s);
    s.ea = ea, s.fmt = fmt, s.depth = depth, s.last_frame = g_frame;
    create_surface_tex(s, nw, nh);
    LOG("rsx", "new %s surface at 0x%08x fmt %u %ux%u", depth ? "depth" : "color", ea, fmt, nw, nh);
    return &s;
}

void create_surface_tex(Surface &s, uint32_t nw, uint32_t nh) {
    bool depth = s.depth;
    uint32_t fmt = s.fmt;
    s.w = nw, s.h = nh;
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
}

// Copy the top-left w x h of one surface into another of the same format.
// Depth cannot be copied by region in D3D11: a pass writes it (stencil is lost).
void copy_depth_region(Surface &dst, Surface &src, uint32_t w, uint32_t h);
void copy_region(Surface &dst, Surface &src, uint32_t w, uint32_t h) {
    w = std::min({w, dst.w, src.w}), h = std::min({h, dst.h, src.h});
    if (dst.depth) { copy_depth_region(dst, src, w, h); return; }
    D3D11_BOX box{0, 0, 0, w, h, 1};
    g_ctx->CopySubresourceRegion(dst.tex, 0, 0, 0, 0, src.tex, 0, &box);
}

// Bring a surface up to date with what was drawn into its proxy.
uint64_t g_surface_serial = 1;
void sync_primary(Surface &s) {
    if (s.proxy && s.newer == 2) copy_region(s, *s.proxy, s.proxy->w, s.proxy->h), s.newer = 0, s.drawn_serial = ++g_surface_serial;
}

// The w x h stand-in for s, holding s's current top-left contents.
Surface *use_proxy(Surface &s, uint32_t w, uint32_t h) {
    if (s.proxy && (s.proxy->w != w || s.proxy->h != h)) {
        sync_primary(s);
        free_surface(*s.proxy);
        delete s.proxy, s.proxy = nullptr;
    }
    if (!s.proxy) {
        s.proxy = new Surface{};
        s.proxy->ea = s.ea, s.proxy->fmt = s.fmt, s.proxy->depth = s.depth;
        create_surface_tex(*s.proxy, w, h);
        s.newer = 1;
        static int logs;
        if (logs++ < 20)
            LOG("rsx", "proxy %ux%u for %s surface 0x%08x (%ux%u)", w, h, s.depth ? "depth" : "color", s.ea, s.w, s.h);
    }
    if (s.newer == 1) copy_region(*s.proxy, s, w, h), s.newer = 0;
    return s.proxy;
}

// After rendering to s (directly or through its proxy).
void mark_drawn(Surface &s, bool via_proxy) {
    s.dirty = true;
    s.drawn_serial = ++g_surface_serial;
    if (s.proxy) s.newer = via_proxy ? 2 : 1;
}

uint32_t dma_location(uint32_t ctx) { return (ctx & 0xf) == 1 ? 1 : 0; }  // 0xfeed0000 local, 0xfeed0001 main

struct Bound {
    Surface *color[4] = {};
    Surface *depth = nullptr;
    uint32_t w = 0, h = 0;
    ID3D11RenderTargetView *rtv[4] = {};   // the views bound (a proxy's when proxied)
    ID3D11DepthStencilView *dsv = nullptr;
    bool proxied[5] = {};                  // colour 0-3, depth
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
    for (int i = 0; i < 4; i++) {
        if (!(mask & (1 << i))) continue;
        uint32_t ea = ea_for(dma_location(reg(dma_m[i])), reg(off_m[i]));
        b.color[i] = get_surface(ea, cfmt, sw, shh, false);
        if (b.color[i]) b.color[i]->pitch = reg(pitch_m[i]);
    }
    if (zfmt) {
        uint32_t ea = ea_for(dma_location(reg(0x0198)), reg(0x0214));          // CONTEXT_DMA_ZETA
        b.depth = get_surface(ea, zfmt, sw, shh, true);
        if (b.depth) b.depth->pitch = reg(0x022c);                               // SURFACE_PITCH_Z
    }
    // Surfaces keep their largest size, so one address reused for a smaller buffer
    // (Killzone 2: the 1024x1024 shadow map as the 640x360 depth for depth of field,
    // next to a 2560x720 G-buffer target) gives views of different sizes, which
    // D3D11 refuses to bind together. Those surfaces render through sw x shh proxies.
    Surface *all[5] = {b.color[0], b.color[1], b.color[2], b.color[3], b.depth};
    bool mismatch = false;
    Surface *first = nullptr;
    for (auto *x : all)
        if (x) { if (!first) first = x; else if (x->w != first->w || x->h != first->h) mismatch = true; }
    ID3D11ShaderResourceView *none[16] = {};
    g_ctx->PSSetShaderResources(0, 16, none);   // a bound surface must not also be bound as a texture
    for (int i = 0; i < 5; i++) {
        Surface *x = all[i];
        if (!x) continue;
        Surface *v = x;
        if (mismatch && (x->w != sw || x->h != shh)) v = use_proxy(*x, sw, shh), b.proxied[i] = true;
        else sync_primary(*x);
        if (i < 4) b.rtv[i] = v->rtv; else b.dsv = v->dsv;
    }
    g_ctx->OMSetRenderTargets(4, b.rtv, b.dsv);
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
    double last_used = 0;          // g_flip_time when last bound
};
std::unordered_map<uint64_t, Tex> g_textures;
double g_flip_time = 0;            // seconds since start, updated at each flip
IDXGIAdapter3 *g_adapter3;         // for video memory use in the fps log

// Leak hunting (KZ2_LEAK_BISECT=1): every 8 s the renderer drops one piece of
// its work, and the flip logs memory, so the growth rate per mode shows which
// part makes the driver allocate. 0 normal, 1 no DrawIndexed, 2 no vertex/index
// ring uploads, 3 no constant uploads, 4 no texture creation/updates, 5 no
// per-draw work at all, 6 also no readbacks.
int leak_mode() {
    static const bool on = getenv("KZ2_LEAK_BISECT") != nullptr;
    return on ? (int)(g_flip_time / 8) % 7 : 0;
}

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
    // X16: one 16-bit value that the RSX returns in every channel (Killzone 2 reads its
    // quarter-size depth through remap 0xee, i.e. from the blue channel); replicated
    // into R16G16B16A16. KZ2_X16_RED_ONLY=1: old behaviour (value in red only).
    static const bool x16_red = getenv("KZ2_X16_RED_ONLY") && atoi(getenv("KZ2_X16_RED_ONLY"));
    if (base == 0x94 && !x16_red) {
        if (!pitch || !linear) pitch = w * 2;
        uint32_t lw = log2u(w), lh = log2u(h);
        const uint8_t *src = (const uint8_t *)mem::ptr(ea);
        row = w * 8;
        out.resize((size_t)row * h);
        for (uint32_t y = 0; y < h; y++)
            for (uint32_t x = 0; x < w; x++) {
                size_t o = linear ? (size_t)y * pitch + (size_t)x * 2 : (size_t)swizzled_offset(x, y, lw, lh) * 2;
                uint16_t v = (uint16_t)(src[o] << 8 | src[o + 1]);
                uint16_t *d = (uint16_t *)&out[(size_t)y * row + (size_t)x * 8];
                d[0] = d[1] = d[2] = d[3] = v;
            }
        return DXGI_FORMAT_R16G16B16A16_UNORM;
    }
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
    sync_primary(s);
    if (!s.dirty || !s.pitch) return;
    mem::track_unarm(ea, s.pitch * s.h);   // one call for the whole range instead of a fault per page
    s.dirty = false;
    ScopedUs timer{g_wbs_us};
    g_wbs_n++;
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
    if (s.copied_serial != s.drawn_serial) g_ctx->CopyResource(s.staging, s.tex), s.copied_serial = s.drawn_serial;
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
    // g_present_vs scales its texture coordinates by uvscale (VS b0). Bind (1, 1):
    // otherwise b0 still holds the game's vertex constants from the last draw, and
    // every helper pass (depth read as colour, G-buffer reinterpretation) sampled
    // its source mirrored or scaled by whatever c[0] happened to be.
    g_ctx->VSSetConstantBuffers(0, 1, &g_cb_unit_uv);
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

// Diagnostics for environment reflections: a black cube map (F11), and the six faces
// of each cube map used in the captured frame (cube_<ea>_<face>.bmp, D3D face order
// +X -X +Y -Y +Z -Z, each as seen from the centre).
ID3D11ShaderResourceView *black_cube() {
    static ID3D11ShaderResourceView *srv;
    if (!srv) {
        D3D11_TEXTURE2D_DESC d{};
        d.Width = d.Height = 1, d.MipLevels = 1, d.ArraySize = 6, d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        d.SampleDesc.Count = 1, d.BindFlags = D3D11_BIND_SHADER_RESOURCE, d.MiscFlags = D3D11_RESOURCE_MISC_TEXTURECUBE;
        uint32_t zero[6] = {};
        D3D11_SUBRESOURCE_DATA sd[6];
        for (int f = 0; f < 6; f++) sd[f] = {&zero[f], 4, 0};
        ID3D11Texture2D *t = nullptr;
        g_dev->CreateTexture2D(&d, sd, &t);
        g_dev->CreateShaderResourceView(t, nullptr, &srv);
        t->Release();
    }
    return srv;
}
ID3D11PixelShader *g_cube_face_ps;
bool make_target(uint32_t w, uint32_t h, DXGI_FORMAT f, ID3D11Texture2D *&tex, ID3D11RenderTargetView *&rtv,
                 ID3D11ShaderResourceView *&srv);
void dump_srv(ID3D11ShaderResourceView *v, const char *path, bool signed_map);
void dump_cube(ID3D11ShaderResourceView *cube, uint32_t ea) {
    static std::set<uint32_t> done;
    static uint32_t frame;
    if (frame != g_frame) done.clear(), frame = g_frame;
    if (!g_cube_face_ps || !done.insert(ea).second) return;
    static ID3D11Texture2D *tex; static ID3D11RenderTargetView *rtv; static ID3D11ShaderResourceView *srv;
    if (!tex && !make_target(128, 128, DXGI_FORMAT_R8G8B8A8_UNORM, tex, rtv, srv)) return;
    for (uint32_t f = 0; f < 6; f++) {
        run_pass(cube, rtv, 128, 128, g_cube_face_ps, f);
        char path[64];
        snprintf(path, sizeof path, "cube_%08x_%u.bmp", ea, f);
        dump_srv(srv, path, false);
    }
}

// Depth copy for proxies (see copy_region): a full-screen pass over the top-left
// w x h of dst writes the source depth. Bound targets and viewport are restored.
void copy_depth_region(Surface &dst, Surface &src, uint32_t w, uint32_t h) {
    ID3D11RenderTargetView *rtv[4];
    ID3D11DepthStencilView *dsv;
    g_ctx->OMGetRenderTargets(4, rtv, &dsv);
    UINT nvp = 1;
    D3D11_VIEWPORT vp;
    g_ctx->RSGetViewports(&nvp, &vp);

    ID3D11ShaderResourceView *none[16] = {};
    g_ctx->PSSetShaderResources(0, 16, none);
    g_ctx->OMSetRenderTargets(0, nullptr, dst.dsv);
    D3D11_VIEWPORT cv{0, 0, (float)w, (float)h, 0, 1};
    g_ctx->RSSetViewports(1, &cv);
    g_ctx->RSSetState(nullptr);
    g_ctx->OMSetBlendState(nullptr, nullptr, 0xffffffff);
    g_ctx->OMSetDepthStencilState(g_ds_always, 0);
    g_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g_ctx->VSSetShader(g_present_vs, nullptr, 0);
    g_ctx->VSSetConstantBuffers(0, 1, &g_cb_unit_uv);
    g_ctx->PSSetShader(g_depth_copy_ps, nullptr, 0);
    g_ctx->PSSetShaderResources(0, 1, &src.srv);
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

// The top-left w x h of surface s as a texture, copied on the GPU - instead of writing
// the whole surface back to guest memory (a CPU stall on the GPU queue) and decoding the
// region from there. Colour surfaces: a region copy in the surface's format. 16-bit depth
// read as X16: a pass writing the depth into all four channels (as decode() does).
ID3D11PixelShader *g_depth_x16_ps;
ID3D11ShaderResourceView *sub_view(Surface &s, uint32_t w, uint32_t h) {
    if (s.sub_w != w || s.sub_h != h || !s.sub_tex) {
        release(s.sub_srv), release(s.sub_rtv), release(s.sub_tex);
        DXGI_FORMAT f = s.depth ? DXGI_FORMAT_R16G16B16A16_UNORM : color_format(s.fmt);
        if (!make_target(w, h, f, s.sub_tex, s.sub_rtv, s.sub_srv)) return nullptr;
        s.sub_w = w, s.sub_h = h, s.sub_serial = 0;
    }
    if (s.sub_serial != s.drawn_serial) {
        if (s.depth) run_pass(s.srv, s.sub_rtv, w, h, g_depth_x16_ps);
        else {
            D3D11_BOX box{0, 0, 0, w, h, 1};
            g_ctx->CopySubresourceRegion(s.sub_tex, 0, 0, 0, 0, s.tex, 0, &box);
        }
        s.sub_serial = s.drawn_serial;
    }
    return s.sub_srv;
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
        if (ds != g_depth_surfaces.end()) sync_primary(ds->second);
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
        if (s != g_surfaces.end()) sync_primary(s->second);
        // Capture: which path serves a texture whose address also holds a depth surface.
        auto trace = [&](const char *how) {
            static int n;
            if ((int)g_frame == g_log_frame && ds != g_depth_surfaces.end() && n++ < 40)
                LOG("rsx", "   texture %08x fmt %02x %ux%u (depth surface %ux%u): %s", ea, base, w, h, ds->second.w,
                    ds->second.h, how);
        };
        // KZ2_SUBRECT_GPU=0: read such regions back through guest memory, as before.
        static const bool subrect = !getenv("KZ2_SUBRECT_GPU") || atoi(getenv("KZ2_SUBRECT_GPU"));
        if (subrect && (fmt & 0x20) && s != g_surfaces.end() && !s->second.depth && pitch == s->second.pitch &&
            s->second.w >= w && s->second.h >= h && (s->second.w != w || s->second.h != h) &&
            same_layout(s->second.fmt, base) && color_format(s->second.fmt) != DXGI_FORMAT_UNKNOWN) {
            trace("GPU region copy of the colour surface");
            if (ID3D11ShaderResourceView *v = sub_view(s->second, w, h)) return v;
        }
        if (subrect && (fmt & 0x20) && base == 0x94 && ds != g_depth_surfaces.end() && ds->second.fmt == 1 &&
            pitch == ds->second.pitch && ds->second.w >= w && ds->second.h >= h) {
            trace("GPU region copy of the Z16 depth surface");
            if (ID3D11ShaderResourceView *v = sub_view(ds->second, w, h)) return v;
        }
        if (s != g_surfaces.end()) {
            Surface &rt = s->second;
            if (rt.w == w && rt.h == h) {
                if (same_layout(rt.fmt, base)) { trace("COLOUR surface at the same address"); return rt.srv; }
                if (color_format(rt.fmt) == DXGI_FORMAT_R8G8B8A8_UNORM && reinterpretable(base))
                    return reinterpret_view(rt, base);
            }
            write_back_surface(ea, rt);
        }
        if (ds_fits && base == 0x85) { trace("depth_as_color"); return depth_as_color(ds->second); }   // A8R8G8B8 view of D24S8
        trace(ds_fits ? "decoded from guest memory (format not handled)" : "decoded from guest memory (size mismatch)");
        if (ds != g_depth_surfaces.end()) write_back_surface(ea, ds->second);
    }

    uint64_t key = hfast(&ea, 4, hfast(&fmtreg, 4, hfast(&rect, 4, hfast(&pitch, 4))));
    Tex &t = g_textures[key];
    t.last_used = g_flip_time;
    if (leak_mode() == 4) return t.srv;   // leak hunting: no texture creation or updates
    uint32_t size = image_size(fmt, w, h, pitch);
    if (t.srv && t.checked_frame == g_frame) return t.srv;
    // Local-memory textures: page write tracking says whether anything wrote the
    // image since we last looked - no need to read it (KZ2_TEX_TRACK=0: always hash).
    // The pages are armed before hashing, so a write during the hash is seen next time.
    uint32_t span = cube ? 6 * image_size(fmt, w, h, pitch) + 6 * 128 : image_size(fmt, w, h, pitch) * 2;   // + mips
    if (ea >= mem::LOCAL_BASE && mem::track_enabled()) {
        if (t.srv && !mem::track_written(ea, span)) { t.checked_frame = g_frame; return t.srv; }
        mem::track_arm(ea, span);
    }
    // Detect CPU-side changes: hash up to 64 KiB spread over the image.
    uint64_t t_hash = host_now_ns();
    uint64_t hv = 1469598103934665603ull;
    const uint8_t *p = (const uint8_t *)mem::ptr(ea);
    if (size <= 65536) hv = hfast(p, size);
    else for (uint32_t k = 0; k < 1024; k++) hv = hfast(p + (uint64_t)size * k / 1024, 64, hv);
    t.checked_frame = g_frame;
    g_hash_ns += host_now_ns() - t_hash, g_hash_n++;
    if (t.srv && t.content == hv) return t.srv;
    t.content = hv;
    ScopedUs timer{g_tex_us};
    g_tex_n++;

    int faces = cube ? 6 : 1;
    // Mip levels follow level 0 in memory, each face holding its full chain (faces
    // 128-byte aligned). Linear (pitched) textures: level 0 only. KZ2_NO_MIPS=1: level 0.
    static const bool no_mips = getenv("KZ2_NO_MIPS") && atoi(getenv("KZ2_NO_MIPS"));
    uint32_t levels = (no_mips || (fmt & 0x20)) ? 1 : std::min(mips, log2u(std::max(w, h)) + 1);
    uint32_t face_stride = 0;
    if (cube) {
        for (uint32_t m = 0, mw = w, mh = h; m < mips; m++, mw = std::max(1u, mw / 2), mh = std::max(1u, mh / 2))
            face_stride += image_size(fmt, mw, mh, (fmt & 0x20) ? pitch : 0);
        face_stride = (face_stride + 127) & ~127u;
    }
    std::vector<std::vector<uint8_t>> data(faces * levels);
    std::vector<D3D11_SUBRESOURCE_DATA> sd(faces * levels);
    DXGI_FORMAT df = DXGI_FORMAT_UNKNOWN;
    for (int f = 0; f < faces; f++)
    for (uint32_t lv = 0, lw = w, lh = h, lofs = 0; lv < levels; lv++) {
        UINT row;
        uint32_t k = f * levels + lv;
        df = decode(ea + f * face_stride + lofs, fmt, lw, lh, pitch, data[k], row);
        sd[k] = {data[k].data(), row, 0};
        lofs += image_size(fmt, lw, lh, (fmt & 0x20) ? pitch : 0);
        lw = std::max(1u, lw / 2), lh = std::max(1u, lh / 2);
        if (lv) continue;
        // Diagnostic only (KZ2_DEBUG_IDENTITY_LUT): an all-zero 64x64 A8R8G8B8 texture
        // (the colour-grading table) becomes an identity ramp, to see what else renders.
        static const bool identity_lut = getenv("KZ2_DEBUG_IDENTITY_LUT") != nullptr;
        if (identity_lut && w == 64 && h == 64 && (fmt & ~0x60u) == 0x85 &&
            std::all_of(data[k].begin(), data[k].end(), [](uint8_t b) { return b == 0; })) {
            for (uint32_t y = 0; y < h; y++)
                for (uint32_t x = 0; x < w; x++) {
                    uint8_t v = (uint8_t)(x * 255 / 63), *d = &data[k][(size_t)y * row + x * 4];
                    d[0] = v, d[1] = v, d[2] = 0, d[3] = v;     // with remap aa93: rgb = v, a (desaturate) = 0
                }
        }
    }
    D3D11_TEXTURE2D_DESC d{};
    d.Width = w, d.Height = h, d.MipLevels = levels, d.ArraySize = faces, d.Format = df;
    d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    d.MiscFlags = cube ? D3D11_RESOURCE_MISC_TEXTURECUBE : 0;
    // Content changed but the shape did not: overwrite the texture in place.
    // Recreating it every time (a texture the game rewrites each frame) left the
    // driver's ~15 MB upload copies behind - the process grew ~30 MB per frame.
    if (t.tex) {
        D3D11_TEXTURE2D_DESC old;
        t.tex->GetDesc(&old);
        if (old.Width == d.Width && old.Height == d.Height && old.Format == d.Format && old.ArraySize == d.ArraySize &&
            old.MiscFlags == d.MiscFlags && old.MipLevels == d.MipLevels) {
            for (uint32_t k = 0; k < sd.size(); k++) g_ctx->UpdateSubresource(t.tex, k, nullptr, sd[k].pSysMem, sd[k].SysMemPitch, 0);
            return t.srv;
        }
        release(t.srv);
        release(t.tex);
    }
    static int big_logs;
    if ((uint64_t)w * h >= (2u << 20) && big_logs++ < 20)
        LOG("rsx", "large texture created: ea 0x%x fmt 0x%x %ux%u", ea, fmt, w, h);
    if (FAILED(g_dev->CreateTexture2D(&d, sd.data(), &t.tex))) {
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
    uint32_t addr = reg(b + 8), filt = reg(b + 0x14), ctl0 = reg(b + 0x0c);
    uint64_t key = hfast(&ctl0, 4, hfast(&filt, 4, ((uint64_t)addr << 32) | shadow));
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
    // Min filter: 1 nearest, 2 linear (level 0 only), 3-6 mipmapped (nearest/linear within
    // and between levels), 7 convolution. LOD: TEXTURE_CONTROL0 min (bits 19-30) and max
    // (7-18) in 4.8 fixed point, filter bits 0-12 a signed 5.8 bias.
    uint32_t minf = (filt >> 16) & 0xff;
    bool min_lin = minf == 2 || minf == 4 || minf == 6 || minf == 7, mip_lin = minf == 5 || minf == 6;
    bool mipmapped = minf >= 3 && minf <= 6;
    int f = (min_lin ? 0x10 : 0) | (lin ? 0x04 : 0) | (mip_lin ? 0x01 : 0);   // D3D11 filter bits
    d.Filter = (D3D11_FILTER)f;
    if (shadow) d.Filter = lin ? D3D11_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT : D3D11_FILTER_COMPARISON_MIN_MAG_MIP_POINT;
    d.MinLOD = ((ctl0 >> 19) & 0xfff) / 256.f;
    d.MaxLOD = mipmapped ? ((ctl0 >> 7) & 0xfff) / 256.f : 0.f;
    if (d.MaxLOD < d.MinLOD) d.MaxLOD = d.MinLOD;
    d.MipLODBias = (float)((int32_t)(filt << 19) >> 19) / 256.f;
    // Anisotropic filtering: TEXTURE_CONTROL0 bits 4-6 = 1x 2x 4x 6x 8x 10x 12x 16x
    // (mipmapped linear minification only). KZ2_NO_ANISO=1 disables.
    static const uint8_t aniso_of[8] = {1, 2, 4, 6, 8, 10, 12, 16};
    static const bool no_aniso = getenv("KZ2_NO_ANISO") && atoi(getenv("KZ2_NO_ANISO"));
    uint32_t aniso = aniso_of[(ctl0 >> 4) & 7];
    if (!no_aniso && !shadow && aniso > 1 && mipmapped && min_lin)
        d.Filter = D3D11_FILTER_ANISOTROPIC, d.MaxAnisotropy = aniso;
    d.AddressU = wrap(addr & 0xf);
    d.AddressV = wrap((addr >> 8) & 0xf);
    d.AddressW = wrap((addr >> 16) & 0xf);
    // RSX zfunc 0..7 (never, less, equal, lequal, greater, notequal, gequal, always) = D3D11 func - 1.
    // The RSX compares the stored depth against the lookup's z (texel OP ref); D3D11
    // compares ref OP texel. Mirror the operator: Killzone 2's shadow maps use GEQUAL
    // (lit where stored >= z); taken as-is every shadow test was inverted.
    // KZ2_SHADOW_CMP_RAW=1: pass the function through unmirrored.
    static const bool cmp_raw = getenv("KZ2_SHADOW_CMP_RAW") && atoi(getenv("KZ2_SHADOW_CMP_RAW"));
    static const uint8_t mirror[8] = {0, 4, 2, 6, 1, 5, 3, 7};
    uint32_t zf = addr >> 28;
    d.ComparisonFunc = shadow ? (D3D11_COMPARISON_FUNC)((cmp_raw ? zf : mirror[zf]) + 1) : D3D11_COMPARISON_NEVER;
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

int g_draw_logged_frame = -1;
void apply_state() {
    // blend
    uint32_t be = reg(0x0310) & 1, sf = reg(0x0314), df = reg(0x0318), eq = reg(0x0320), mask = reg(0x0324);
    // Blending and write masks are per render target on the RSX: target 0 uses BLEND_ENABLE
    // and COLOR_MASK, targets 1-3 SET_BLEND_ENABLE_MRT (0x12c, bits 1-3) and
    // SET_COLOR_MASK_MRT (0x370: A,R,G,B bits from 4 * target). Killzone 2 packs normals as
    // raw half floats into target 2: a decal blending into the albedo must not blend those
    // bytes (random normals: sparkling noise on walls). The MRT mask counts only once the
    // game has set it. KZ2_MRT_STATE=0: target 0's state for all.
    static const bool mrt_state = !getenv("KZ2_MRT_STATE") || atoi(getenv("KZ2_MRT_STATE"));
    static bool mrt_mask_seen;
    uint32_t mrt_be = mrt_state ? reg(0x012c) : 0, mrt_mask = mrt_state ? reg(0x0370) : 0;
    if (mrt_mask) mrt_mask_seen = true;
    uint32_t key_mrt[3] = {mrt_state, mrt_be, mrt_mask_seen ? mrt_mask : 0xffffffffu};
    uint64_t bk = hfast(key_mrt, sizeof key_mrt, hfast(&be, 4, hfast(&sf, 4, hfast(&df, 4, hfast(&eq, 4, hfast(&mask, 4))))));
    auto &bs = g_blend[bk];
    if (!bs) {
        D3D11_BLEND_DESC d{};
        d.IndependentBlendEnable = TRUE;
        UINT8 wm0 = ((mask >> 16) & 1 ? 1 : 0) | ((mask >> 8) & 1 ? 2 : 0) | (mask & 1 ? 4 : 0) | ((mask >> 24) & 1 ? 8 : 0);
        for (int k = 0; k < 4; k++) {
            auto &t = d.RenderTarget[k];
            t.BlendEnable = !mrt_state ? be : k == 0 ? be : (mrt_be >> k) & 1;
            t.SrcBlend = blend_factor(sf & 0xffff, false);
            t.DestBlend = blend_factor(df & 0xffff, false);
            t.BlendOp = blend_op(eq & 0xffff);
            t.SrcBlendAlpha = blend_factor(sf >> 16, true);
            t.DestBlendAlpha = blend_factor(df >> 16, true);
            t.BlendOpAlpha = blend_op(eq >> 16);
            t.RenderTargetWriteMask = wm0;
            if (k && mrt_state && mrt_mask_seen) {
                uint32_t m = mrt_mask >> (4 * k);
                t.RenderTargetWriteMask = ((m >> 1) & 1 ? 1 : 0) | ((m >> 2) & 1 ? 2 : 0) | ((m >> 3) & 1 ? 4 : 0) | (m & 1 ? 8 : 0);
            }
        }
        g_dev->CreateBlendState(&d, &bs);
    }
    uint32_t bc = reg(0x031c);
    float bcf[4] = {((bc >> 16) & 255) / 255.f, ((bc >> 8) & 255) / 255.f, (bc & 255) / 255.f, (bc >> 24) / 255.f};
    g_ctx->OMSetBlendState(bs, bcf, 0xffffffff);

    // depth / stencil
    uint32_t dz[] = {reg(0x0a74) & 1, reg(0x0a6c), reg(0x0a70) & 1, reg(0x0328) & 1, reg(0x032c), reg(0x0330),
                     reg(0x0338), reg(0x033c), reg(0x0340), reg(0x0344)};
    auto &ds = g_depth[hfast(dz, sizeof dz)];
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

    // rasterizer: culling off for now, scissor on. Front face (SV_IsFrontFace, used by
    // two-sided materials to flip normals): we rasterise vertically mirrored relative to
    // the RSX (see origin_bottom), which reverses winding - an RSX counter-clockwise
    // front face (0x1834 = 0x901) appears clockwise here, and vice versa.
    static const bool ff_raw = getenv("KZ2_FRONT_FACE_RAW") != nullptr;   // experiment: no mirror correction
    bool rsx_ccw = reg(0x1834) != 0x0900;
    bool front_ccw = ff_raw ? rsx_ccw : !rsx_ccw;
    uint32_t sc = front_ccw ? 3 : 1;
    // Polygon offset (decals on walls): enable 0x0a68, slope factor 0x0a78, units 0x0a7c. RSX and
    // D3D both multiply units by the depth format's smallest step. KZ2_NO_DEPTH_BIAS=1: off.
    static const bool no_bias = getenv("KZ2_NO_DEPTH_BIAS") && atoi(getenv("KZ2_NO_DEPTH_BIAS"));
    bool bias_on = !no_bias && (reg(0x0a68) & 1);
    uint32_t rkey[3] = {sc, bias_on ? reg(0x0a78) : 0, bias_on ? reg(0x0a7c) : 0};
    auto &rs = g_raster[hfast(rkey, sizeof rkey)];
    if (!rs) {
        D3D11_RASTERIZER_DESC d{};
        d.FillMode = D3D11_FILL_SOLID;
        d.CullMode = D3D11_CULL_NONE;
        d.FrontCounterClockwise = front_ccw;
        d.DepthClipEnable = TRUE;
        d.ScissorEnable = TRUE;
        if (bias_on) d.SlopeScaledDepthBias = regf(0x0a78), d.DepthBias = (INT)std::lround(regf(0x0a7c));
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
        h = hfast(&vp_ucode[i * 4], 16, h);
        if (vp_ucode[i * 4 + 3] & 1) break;
    }
    // Attribute formats are compiled into the shader (a decode per attribute, see
    // vp.cpp fetch_code): 0 constant, RSX type | size << 4, 8 host float4 (CPU fetch).
    static const bool cpu_vertex = getenv("KZ2_CPU_VERTEX") && atoi(getenv("KZ2_CPU_VERTEX"));
    uint8_t afmt[16] = {};
    for (int a = 0; a < 16; a++) {
        uint32_t fmt = reg(0x1740 + a * 4), type = fmt & 0xf, size = (fmt >> 4) & 0xf;
        if (cpu_vertex) afmt[a] = 8;
        else if ((reg(0x1ff0) & (1 << a)) && size && type >= 1 && type <= 7) afmt[a] = (uint8_t)(type | size << 4);
    }
    h = hfast(afmt, 16, h);
    g_vs_hash = h;
    auto it = g_vs.find(h);
    if (it != g_vs.end()) { g_cur_win = g_vs_win[h]; return it->second; }
    VpInfo vi = translate_vp(start, afmt);
    ID3D11VertexShader *vs = nullptr;
    if (ID3DBlob *b = compile(vi.hlsl, "vs_5_0", h)) {
        g_dev->CreateVertexShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &vs);
        b->Release();
    }
    g_vs[h] = vs;
    g_vs_hlsl[h] = vi.hlsl;
    g_cur_win = g_vs_win[h] = {vi.lo_base, vi.lo_count, vi.hi_base, vi.hi_count};
    return vs;
}

PsEntry *pixel_shader(uint32_t fp_ea, const uint8_t texdim[16], const uint16_t remap[16], const uint8_t gamma[16]) {
    const FpScan &sc = scan_fp(fp_ea);
    uint32_t control = reg(0x1d60);
    uint32_t alpha = (reg(0x0304) & 1) ? reg(0x0308) : 0;
    // SET_SHADER_PACKER (0x1fc4): sRGB framebuffer writes. KZ2_NO_SRGB_OUT=1 ignores it.
    static const bool no_srgb = getenv("KZ2_NO_SRGB_OUT") && atoi(getenv("KZ2_NO_SRGB_OUT"));
    uint32_t srgb_out = !no_srgb && reg(0x1fc4) != 0;
    uint64_t h = hfast(&srgb_out, 4, hfast(gamma, 16, hfast(remap, 32, hfast(texdim, 16, hfast(&control, 4, hfast(&alpha, 4, sc.hash))))));
    auto it = g_ps.find(h);
    if (it != g_ps.end()) return &it->second;
    FpKey key{fp_ea, control, {}, alpha, {}, {}, srgb_out != 0};
    memcpy(key.texdim, texdim, 16);
    memcpy(key.remap, remap, 32);
    memcpy(key.gamma, gamma, 16);
    FpInfo fi = translate_fp(key);
    // Debug: KZ2_DEBUG_PS=<hash prefix> replaces that shader's colour output with
    // KZ2_DEBUG_EXPR (an HLSL float4 over its registers), alpha 1 - to see intermediates.
    if (const char *dbg = getenv("KZ2_DEBUG_PS")) {
        char hs[17];
        snprintf(hs, sizeof hs, "%016llx", (unsigned long long)h);
        if (*dbg && !strncmp(hs, dbg, strlen(dbg))) {
            const char *ex = getenv("KZ2_DEBUG_EXPR");
            std::string expr = ex ? ex : "float4(r[3].x, saturate(-r[0].x / 100), saturate(-i.o8.z / 100), 1)";
            size_t at = fi.hlsl.find("  return o;");
            if (at != std::string::npos) fi.hlsl.insert(at, "  o.c0 = " + expr + ";\n");
            h ^= 0xdeb6deb6ull;   // a separate shader-cache entry
            LOG("rsx", "debug output for pixel shader %s: %s", hs, expr.c_str());
        }
    }
    PsEntry e{nullptr, sc.consts, fi.hlsl, control, false};
    if (ID3DBlob *b = compile(fi.hlsl, "ps_5_0", h)) {
        g_dev->CreatePixelShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &e.ps);
        b->Release();
    }
    return &(g_ps[h] = e);
}

// Debug: write a RGBA8 / RGBA16F colour surface to <name>_f<fmt>.bmp (half floats clamped to 0..1).
void dump_surface(Surface &s, const char *name, bool alpha = false) {
    sync_primary(s);
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
    snprintf(path, sizeof path, alpha ? "%s_f%x_alpha.bmp" : "%s_f%x.bmp", name, s.fmt);
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
                    if (alpha && !half) { c[k] = p[x * 4 + 3]; continue; }   // alpha as grey
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

// Capture: write any 2D texture view to a BMP as the shaders see it (first slice,
// first mip). 8-bit colour as is; float formats clamped to [0,1]; depth as grey.
int g_pass_dumps;
void dump_srv(ID3D11ShaderResourceView *v, const char *path, bool signed_map = false) {
    if (!v) return;
    D3D11_SHADER_RESOURCE_VIEW_DESC vd;
    v->GetDesc(&vd);
    ID3D11Resource *res = nullptr;
    v->GetResource(&res);
    ID3D11Texture2D *tex = nullptr;
    if (!res || FAILED(res->QueryInterface(__uuidof(ID3D11Texture2D), (void **)&tex))) { release(res); return; }
    res->Release();
    D3D11_TEXTURE2D_DESC d;
    tex->GetDesc(&d);
    DXGI_FORMAT f = vd.Format;
    int bpp = f == DXGI_FORMAT_R8G8B8A8_UNORM || f == DXGI_FORMAT_B8G8R8A8_UNORM || f == DXGI_FORMAT_R24_UNORM_X8_TYPELESS ? 4
            : f == DXGI_FORMAT_R16G16B16A16_FLOAT ? 8 : f == DXGI_FORMAT_R32G32B32A32_FLOAT ? 16
            : f == DXGI_FORMAT_R16_UNORM ? 2 : f == DXGI_FORMAT_R16G16_FLOAT ? 4 : f == DXGI_FORMAT_R16G16B16A16_UNORM ? 8 : 0;
    if (!bpp || d.SampleDesc.Count != 1) { tex->Release(); return; }
    D3D11_TEXTURE2D_DESC sd = d;
    sd.MipLevels = 1, sd.ArraySize = 1, sd.Usage = D3D11_USAGE_STAGING, sd.BindFlags = 0, sd.MiscFlags = 0;
    sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Texture2D *st = nullptr;
    if (FAILED(g_dev->CreateTexture2D(&sd, nullptr, &st))) { tex->Release(); return; }
    g_ctx->CopySubresourceRegion(st, 0, 0, 0, 0, tex, 0, nullptr);
    tex->Release();
    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(g_ctx->Map(st, 0, D3D11_MAP_READ, 0, &m))) { st->Release(); return; }
    if (FILE *fp = fopen(path, "wb")) {
        uint32_t sz = d.Width * d.Height * 4;
        uint8_t hdr[54] = {'B', 'M'};
        *(uint32_t *)&hdr[2] = 54 + sz, *(uint32_t *)&hdr[10] = 54, *(uint32_t *)&hdr[14] = 40;
        *(int32_t *)&hdr[18] = d.Width, *(int32_t *)&hdr[22] = -(int32_t)d.Height;
        *(uint16_t *)&hdr[26] = 1, *(uint16_t *)&hdr[28] = 32;
        fwrite(hdr, 1, 54, fp);
        std::vector<uint8_t> row(d.Width * 4);
        auto u8 = [](float x) { return (uint8_t)(std::clamp(x, 0.f, 1.f) * 255.f + .5f); };
        for (UINT y = 0; y < d.Height; y++) {
            const uint8_t *p = (const uint8_t *)m.pData + y * m.RowPitch + 0;
            for (UINT x = 0; x < d.Width; x++) {
                const uint8_t *q = p + x * bpp;
                uint8_t c[3] = {};
                if (f == DXGI_FORMAT_R8G8B8A8_UNORM) c[0] = q[0], c[1] = q[1], c[2] = q[2];
                else if (f == DXGI_FORMAT_B8G8R8A8_UNORM) c[0] = q[2], c[1] = q[1], c[2] = q[0];
                else if (f == DXGI_FORMAT_R24_UNORM_X8_TYPELESS) {
                    uint32_t w; memcpy(&w, q, 4); float z = (w & 0xffffff) / 16777215.f;
                    c[0] = c[1] = c[2] = u8(1 - (1 - z) * 50);    // stretched: depth bunches near 1
                } else if (f == DXGI_FORMAT_R16_UNORM || f == DXGI_FORMAT_R16G16B16A16_UNORM) {
                    uint16_t w; memcpy(&w, q, 2); c[0] = c[1] = c[2] = u8(w / 65535.f);
                } else if (f == DXGI_FORMAT_R16G16B16A16_FLOAT || f == DXGI_FORMAT_R16G16_FLOAT) {
                    int n = f == DXGI_FORMAT_R16G16_FLOAT ? 2 : 3;
                    for (int k = 0; k < n; k++) { uint16_t hv; memcpy(&hv, q + 2 * k, 2); c[k] = u8(std::fabs(h2f(hv))); }
                } else {
                    for (int k = 0; k < 3; k++) {
                        float fv; memcpy(&fv, q + 4 * k, 4);
                        c[k] = u8(signed_map ? fv * 0.5f + 0.5f : std::fabs(fv));   // signed: -1..1 -> 0..1
                    }
                }
                row[x * 4] = c[2], row[x * 4 + 1] = c[1], row[x * 4 + 2] = c[0], row[x * 4 + 3] = 255;
            }
            fwrite(row.data(), 1, row.size(), fp);
        }
        fclose(fp);
    }
    // 8-bit RGBA: the alpha channel as a second, grey image (<path minus .bmp>_alpha.bmp).
    if (f == DXGI_FORMAT_R8G8B8A8_UNORM || f == DXGI_FORMAT_B8G8R8A8_UNORM) {
        std::string ap(path);
        if (ap.size() > 4) ap.insert(ap.size() - 4, "_alpha");
        if (FILE *fp = fopen(ap.c_str(), "wb")) {
            uint32_t sz = d.Width * d.Height * 4;
            uint8_t hdr[54] = {'B', 'M'};
            *(uint32_t *)&hdr[2] = 54 + sz, *(uint32_t *)&hdr[10] = 54, *(uint32_t *)&hdr[14] = 40;
            *(int32_t *)&hdr[18] = d.Width, *(int32_t *)&hdr[22] = -(int32_t)d.Height;
            *(uint16_t *)&hdr[26] = 1, *(uint16_t *)&hdr[28] = 32;
            fwrite(hdr, 1, 54, fp);
            std::vector<uint8_t> row(d.Width * 4);
            for (UINT y = 0; y < d.Height; y++) {
                const uint8_t *p = (const uint8_t *)m.pData + y * m.RowPitch;
                for (UINT x = 0; x < d.Width; x++) row[x * 4] = row[x * 4 + 1] = row[x * 4 + 2] = p[x * 4 + 3], row[x * 4 + 3] = 255;
                fwrite(row.data(), 1, row.size(), fp);
            }
            fclose(fp);
        }
    }
    g_ctx->Unmap(st, 0);
    st->Release();
}

// Per-stage time of draw() (flip log): where the RSX thread's per-draw cost goes.
uint64_t g_seg_ns[8], g_seg_t;
inline void lap(int i) { uint64_t n = host_now_ns(); g_seg_ns[i] += n - g_seg_t; g_seg_t = n; }

/* ---------------- occlusion queries ----------------
   ZPASS_PIXEL_CNT reports: CLEAR_REPORT_VALUE (0x17c8) starts a count, draws while
   SET_ZPASS_PIXEL_COUNT_ENABLE (0x1d84) is on add the samples that pass depth/stencil,
   GET_REPORT (0x1800) writes the count since the clear. Each enabled stretch is one
   D3D11 occlusion query; a report's queries are resolved without stalling the GPU
   (polled every draw, forced at the flip), and the value is written to the report
   area when known. Killzone 2 fades light sprites (coronas) by these counts; answering
   "always visible" drew them through walls. KZ2_REPORT_VISIBLE=1: the old answer. */
std::vector<ID3D11Query *> g_occ_since_clear;   // ended stretches since the last clear
ID3D11Query *g_occ_active = nullptr;            // current stretch, while counting is on
bool g_occ_enabled = false;
struct PendingReport { uint32_t ea; std::vector<ID3D11Query *> queries; uint64_t sum = 0; size_t done = 0; };
std::vector<PendingReport> g_reports;
std::vector<ID3D11Query *> g_query_pool;
std::unordered_map<ID3D11Query *, int> g_query_refs;   // a query can serve several reports
uint64_t g_occ_reports = 0, g_occ_visible = 0;         // per flip, for the log
ID3D11Query *occ_new() {
    ID3D11Query *q = nullptr;
    if (!g_query_pool.empty()) q = g_query_pool.back(), g_query_pool.pop_back();
    else {
        D3D11_QUERY_DESC d{D3D11_QUERY_OCCLUSION, 0};
        g_dev->CreateQuery(&d, &q);
    }
    return q;
}
void occ_unref(ID3D11Query *q) { if (--g_query_refs[q] <= 0) g_query_refs.erase(q), g_query_pool.push_back(q); }
void occ_end_active() {
    if (!g_occ_active) return;
    g_ctx->End(g_occ_active);
    g_query_refs[g_occ_active]++;
    g_occ_since_clear.push_back(g_occ_active);
    g_occ_active = nullptr;
}
void occ_begin() {
    if (g_occ_active || !g_occ_enabled) return;
    if ((g_occ_active = occ_new())) g_ctx->Begin(g_occ_active);
}
void occ_clear() {
    occ_end_active();
    for (ID3D11Query *q : g_occ_since_clear) occ_unref(q);
    g_occ_since_clear.clear();
    occ_begin();
}
void write_report(uint32_t ea, uint32_t value) {
    uint64_t t = (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(
                     std::chrono::steady_clock::now().time_since_epoch()).count();
    st32(ea, (uint32_t)(t >> 32)), st32(ea + 4, (uint32_t)t), st32(ea + 8, value), st32(ea + 12, 0);
}
// Resolve finished reports; wait = true blocks until all are (flip, conditional render).
void occ_poll(bool wait) {
    for (size_t i = 0; i < g_reports.size();) {
        PendingReport &r = g_reports[i];
        while (r.done < r.queries.size()) {
            UINT64 n = 0;
            HRESULT hr;
            while ((hr = g_ctx->GetData(r.queries[r.done], &n, sizeof n, wait ? 0 : D3D11_ASYNC_GETDATA_DONOTFLUSH)) == S_FALSE &&
                   wait) {}
            if (hr != S_OK) break;
            r.sum += n, r.done++;
        }
        if (r.done < r.queries.size()) { i++; continue; }
        write_report(r.ea, (uint32_t)std::min<uint64_t>(r.sum, 0xffffffffu));
        g_occ_reports++, g_occ_visible += r.sum != 0;
        for (ID3D11Query *q : r.queries) occ_unref(q);
        g_reports[i] = std::move(g_reports.back());
        g_reports.pop_back();
    }
}
bool g_render_enabled = true;   // SET_RENDER_ENABLE (conditional rendering)

void draw() {
    if (!g_ok) return;
    if (!g_reports.empty()) occ_poll(false);
    if (!g_render_enabled) { g_skip[0]++; return; }
    g_seg_t = host_now_ns();
    if (g_ranges.empty()) { g_skip[0]++; return; }
    g_draws_this_frame++;
    if (leak_mode() >= 5) return;
    // debug: KZ2_SKIP_DRAWS=a,b,c skips those draw indices in every frame
    static std::vector<int> skip = [] {
        std::vector<int> v;
        if (const char *e = getenv("KZ2_SKIP_DRAWS"))
            for (const char *p = e; *p;) { v.push_back(atoi(p)); while (*p && *p != ',') p++; if (*p) p++; }
        return v;
    }();
    for (int k : skip) if (k == g_draws_this_frame) return;
    Bound b = bind_surfaces();
    lap(0);
    if (!b.w) { g_skip[1]++; return; }
    if (b.color[0] && b.color[1] && b.color[2] && b.color[3]) g_scene_draws++;

    // vertices + indices
    static std::vector<uint32_t> src_idx;      // kept between draws: no allocation per draw
    src_idx.clear();
    uint32_t vmin = ~0u, vmax = 0;
    static std::vector<uint32_t> list;
    list.clear();
    // Fast path for the common case, one indexed triangle list (libgcm cuts it into
    // back-to-back ranges): read the big-endian indices straight into the final list,
    // then rebase them, instead of building intermediate index vectors. A restart
    // marker in the stream falls back to the general path. KZ2_SLOW_INDEX=1: always general.
    static const bool slow_index = getenv("KZ2_SLOW_INDEX") && atoi(getenv("KZ2_SLOW_INDEX"));
    bool fast = false;
    if (g_indexed && g_prim == 5 && !slow_index) {
        bool chained = true;
        uint64_t total = 0;
        for (size_t i = 0; i < g_ranges.size(); i++) {
            if (i && g_ranges[i].first != g_ranges[i - 1].first + g_ranges[i - 1].count) chained = false;
            total += g_ranges[i].count;
        }
        total -= total % 3;
        if (chained && total) {
            uint32_t loc = reg(0x1820) & 0xf, type = (reg(0x1820) >> 4) & 0xf;
            uint32_t ea = ea_for(loc ? 1 : 0, reg(0x181c)) + (type ? 2 : 4) * g_ranges[0].first;
            bool restart = reg(0x1dac) & 1;
            uint32_t rval = reg(0x1db0);
            const uint8_t *p = (const uint8_t *)mem::ptr(ea);
            list.resize((size_t)total);
            fast = true;
            // Eight indices at a time: swap bytes, widen, track min and max, look for restarts.
            size_t i0 = 0;
            {
                const __m256i rv = _mm256_set1_epi32((int)rval);
                const __m128i sw16 = _mm_setr_epi8(1, 0, 3, 2, 5, 4, 7, 6, 9, 8, 11, 10, 13, 12, 15, 14);
                const __m256i sw32 = _mm256_setr_epi8(3, 2, 1, 0, 7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12,
                                                      3, 2, 1, 0, 7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12);
                __m256i vlo = _mm256_set1_epi32(-1), vhi = _mm256_setzero_si256();
                for (; i0 + 8 <= total; i0 += 8) {
                    __m256i x = type ? _mm256_cvtepu16_epi32(_mm_shuffle_epi8(_mm_loadu_si128((const __m128i *)(p + 2 * i0)), sw16))
                                     : _mm256_shuffle_epi8(_mm256_loadu_si256((const __m256i *)(p + 4 * i0)), sw32);
                    if (restart && _mm256_movemask_epi8(_mm256_cmpeq_epi32(x, rv))) { fast = false; break; }
                    _mm256_storeu_si256((__m256i *)(list.data() + i0), x);
                    vlo = _mm256_min_epu32(vlo, x), vhi = _mm256_max_epu32(vhi, x);
                }
                alignas(32) uint32_t lo8[8], hi8[8];
                _mm256_store_si256((__m256i *)lo8, vlo), _mm256_store_si256((__m256i *)hi8, vhi);
                for (int k = 0; k < 8; k++) vmin = std::min(vmin, lo8[k]), vmax = std::max(vmax, hi8[k]);
            }
            for (size_t i = i0; fast && i < total; i++) {
                uint32_t v;
                if (type) v = (uint32_t)p[2 * i] << 8 | p[2 * i + 1];
                else { memcpy(&v, p + 4 * i, 4); v = __builtin_bswap32(v); }
                if (restart && v == rval) { fast = false; break; }
                list[i] = v;
                vmin = std::min(vmin, v), vmax = std::max(vmax, v);
            }
            if (!fast) list.clear(), vmin = ~0u, vmax = 0;
        }
    }
    if (fast) {
    } else if (g_indexed) {
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
    if (vmin > vmax || vmax - vmin > 4 * 1024 * 1024) { g_skip[2]++; return; }
    uint32_t nv = vmax - vmin + 1;
    UINT base_vertex = 0, start_index = 0;   // where this draw's data sits in the rings
    // Vertex attributes for the shader (VX cbuffer): byte offset of vertex vmin in the
    // vertex ring, stride, type | size << 4, enabled; constant values when disabled.
    uint32_t vattr[16][4] = {};
    // GPU vertex fetch: copy the raw guest bytes of each attribute's vertex range
    // (overlapping ranges - interleaved attributes - merged into one copy); the vertex
    // shader decodes them (vp.cpp fetch). KZ2_CPU_VERTEX=1: decode on the CPU instead.
    static const bool cpu_vertex = getenv("KZ2_CPU_VERTEX") && atoi(getenv("KZ2_CPU_VERTEX"));
    if (leak_mode() != 2 && !cpu_vertex) {
        ScopedUs timer{g_fetch_us};
        g_fetch_verts += nv;
        g_fetch_used += fast ? list.size() : src_idx.size();
        uint32_t mask = reg(0x1ff0), base_off = reg(0x1738), base_idx = reg(0x173c);
        struct Span { uint64_t lo, hi; int attr; };
        Span spans[16];
        int ns = 0;
        static const uint8_t comp_bytes[8] = {0, 2, 4, 2, 1, 2, 4, 1};
        for (int a = 0; a < 16; a++) {
            uint32_t fmt = reg(0x1740 + a * 4), type = fmt & 0xf, size = (fmt >> 4) & 0xf, stride = (fmt >> 8) & 0xff;
            if (!(mask & (1 << a)) || !size || type < 1 || type > 7) continue;
            uint32_t off = reg(0x1680 + a * 4);
            uint32_t ea = ea_for(off >> 31, (off & 0x7fffffff) + base_off);
            uint32_t elem = type == 6 ? 4 : comp_bytes[type] * size;
            uint64_t lo = (uint64_t)ea + (uint64_t)(base_idx + vmin) * stride;
            uint64_t hi = (uint64_t)ea + (uint64_t)(base_idx + vmax) * stride + elem;
            spans[ns++] = {lo, hi, a};
            vattr[a][1] = stride, vattr[a][2] = type | size << 4, vattr[a][3] = 1;
        }
        std::sort(spans, spans + ns, [](const Span &x, const Span &y) { return x.lo < y.lo; });
        // Merge overlapping spans; each merged span starts 16-byte aligned in the ring.
        struct Run { uint64_t lo, hi; uint32_t dst; };
        Run runs[16];
        int nr = 0;
        uint32_t total = 0;
        int run_of[16];
        for (int i = 0; i < ns; i++) {
            if (nr && spans[i].lo < runs[nr - 1].hi) runs[nr - 1].hi = std::max(runs[nr - 1].hi, spans[i].hi);
            else runs[nr++] = {spans[i].lo, spans[i].hi, 0};
            run_of[i] = nr - 1;
        }
        for (int r = 0; r < nr; r++) runs[r].dst = total, total += (uint32_t)((runs[r].hi - runs[r].lo + 15) & ~15ull);
        ScopedUs map_timer{g_map_us};
        uint8_t *vd = total ? (uint8_t *)ring_map(g_vring, total, 16, D3D11_BIND_SHADER_RESOURCE, true, &base_vertex) : nullptr;
        if (total && !vd) { g_skip[3]++; return; }
        for (int r = 0; r < nr; r++) memcpy(vd + runs[r].dst, mem::ptr((uint32_t)runs[r].lo), runs[r].hi - runs[r].lo);
        if (total) g_ctx->Unmap(g_vring.buf, 0);
        for (int i = 0; i < ns; i++)
            vattr[spans[i].attr][0] = base_vertex * 16 + runs[run_of[i]].dst + (uint32_t)(spans[i].lo - runs[run_of[i]].lo);
        base_vertex = 0;
    } else if (leak_mode() != 2) {
        ScopedUs timer{g_fetch_us};
        g_fetch_verts += nv;
        g_fetch_used += fast ? list.size() : src_idx.size();
        // Decode into ordinary memory, then copy once: the mapped buffer is
        // write-combined, where scattered partial writes are very slow.
        static std::vector<float> scratch;
        if (scratch.size() < (size_t)nv * 64) scratch.resize((size_t)nv * 64);
        for (uint32_t i = 0; i < nv; i++) fetch_vertex(scratch.data() + (size_t)i * 64, vmin + i);
        ScopedUs map_timer{g_map_us};
        void *vd = ring_map(g_vring, nv * 256, 256, D3D11_BIND_SHADER_RESOURCE, true, &base_vertex);
        if (!vd) { g_skip[3]++; return; }
        memcpy(vd, scratch.data(), (size_t)nv * 256);
        g_ctx->Unmap(g_vring.buf, 0);
        // The shader reads host float4s: vertex i's attribute k at (base + i) * 256 + k * 16.
        for (int a = 0; a < 16; a++) vattr[a][0] = base_vertex * 256 + a * 16, vattr[a][1] = 256, vattr[a][2] = 8, vattr[a][3] = 1;
        base_vertex = 0;
    }

    D3D11_PRIMITIVE_TOPOLOGY topo = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    // split at restart markers / range boundaries for strip-like primitives
    std::vector<uint32_t> run;
    size_t at = 0;
    auto flush_run = [&] {
        for (auto &v : run) v -= vmin;
        if (!run.empty()) topo = gen_indices(g_prim, run.data(), (uint32_t)run.size(), list);
        run.clear();
    };
    // A draw's ranges are usually one index stream cut into pieces: DRAW_INDEX_ARRAY and
    // DRAW_ARRAYS hold at most 256 indices each, so libgcm splits large draws into
    // back-to-back ranges. Assemble primitives across such ranges; break only at restart
    // markers or where a range does not continue the previous one. (Per range, a
    // triangle list dropped 256 % 3 = 1 index per piece and every later triangle was
    // built from the wrong vertices.)
    if (fast) {
        for (auto &v : list) v -= vmin;
    } else {
        uint32_t next_first = ~0u;
        for (auto &r : g_ranges) {
            if (r.first != next_first) flush_run();
            next_first = r.first + r.count;
            for (uint32_t i = 0; i < r.count && at < src_idx.size(); i++, at++) {
                if (src_idx[at] == ~0u) { flush_run(); continue; }
                run.push_back(src_idx[at]);
            }
        }
        flush_run();
    }
    if (list.empty()) { g_skip[4]++; return; }
    if (leak_mode() != 2) {
        uint32_t *id = (uint32_t *)ring_map(g_iring, (UINT)list.size() * 4, 4, D3D11_BIND_INDEX_BUFFER, false, &start_index);
        if (!id) { g_skip[3]++; return; }
        memcpy(id, list.data(), list.size() * 4);
        g_ctx->Unmap(g_iring.buf, 0);
    }

    lap(1);
    // shaders
    ID3D11VertexShader *vs = vertex_shader();
    lap(7);
    uint32_t sp = reg(0x08e4);
    uint32_t fp_ea = ea_for((sp & 3) == 2 ? 1 : 0, sp & ~3u);
    uint8_t texdim[16] = {};
    ID3D11ShaderResourceView *srv[16] = {};
    ID3D11SamplerState *smp[16] = {};
    // Games leave textures enabled that the program never samples (Killzone 2 keeps
    // its G-buffer bound while rendering into it); binding one costs a decode, a
    // readback or a snapshot copy of a render target, so bind only sampled units.
    uint16_t sampled = scan_fp(fp_ea).tex_mask;
    for (int t = 0; t < 16; t++) {
        if (!(reg(0x1a0c + t * 32) & 0x80000000) || !(sampled & (1 << t))) continue;
        TexDim d;
        srv[t] = get_texture(t, d);
        // Capture: raw bytes (level 0) of each compressed 2D texture, to check the block
        // layout offline (tex_<ea>_<fmt>_<w>x<h>.bin).
        if ((int)g_frame == g_log_frame && d != TEX_CUBE) {
            uint32_t fr = reg(0x1a04 + t * 32), base = (fr >> 8) & 0x9f, rect = reg(0x1a18 + t * 32);
            uint32_t tea = ea_for((fr & 3) == 2, reg(0x1a00 + t * 32));
            static std::set<uint32_t> raw_done;
            static uint32_t raw_frame;
            if (raw_frame != g_frame) raw_done.clear(), raw_frame = g_frame;
            if (base >= 0x86 && base <= 0x88 && raw_done.insert(tea).second) {
                char path[80];
                snprintf(path, sizeof path, "tex_%08x_%02x_%ux%u.bin", tea, base, rect >> 16, rect & 0xffff);
                if (FILE *fp = fopen(path, "wb")) {
                    fwrite(mem::ptr(tea), 1, image_size(base, rect >> 16, rect & 0xffff, 0), fp);
                    fclose(fp);
                }
            }
        }
        if (d == TEX_CUBE) {
            if ((int)g_frame == g_log_frame && srv[t]) dump_cube(srv[t], ea_for((reg(0x1a04 + t * 32) & 3) == 2, reg(0x1a00 + t * 32)));
            if (g_no_cube) srv[t] = black_cube();
        }
        texdim[t] = d;
        if ((reg(0x1a04 + t * 32) >> 8) & 0x40) texdim[t] |= TEX_UNNORM;
        // Convolution filter (min 7 / mag 4): a hardware kernel over neighbouring
        // texels, done in the shader (see fp.cpp). Killzone 2 resolves its 2x
        // multisampled frame with the quincunx kernel.
        uint32_t filt = reg(0x1a14 + t * 32);
        if (((filt >> 16) & 0xff) == 7 || ((filt >> 24) & 0xff) == 4) {
            uint32_t k = (filt >> 13) & 7;
            if (k == 1 || k == 3) texdim[t] |= TEX_QUINCUNX;
            else if (k == 2) texdim[t] |= TEX_GAUSS;
        }
        smp[t] = get_sampler(t, texdim[t] & TEX_SHADOW);
    }
    lap(2);
    uint16_t remap[16];
    for (int t = 0; t < 16; t++) remap[t] = (reg(0x1a0c + t * 32) & 0x80000000) ? reg(0x1a10 + t * 32) & 0xffff : 0xaae4;
    uint8_t gamma[16];   // per-channel sRGB decode (TEXTURE_ADDRESS bits 20-23); KZ2_NO_TEX_GAMMA=1 ignores it
    static const bool no_gamma = getenv("KZ2_NO_TEX_GAMMA") != nullptr;
    for (int t = 0; t < 16; t++)
        gamma[t] = (!no_gamma && (reg(0x1a0c + t * 32) & 0x80000000)) ? (reg(0x1a08 + t * 32) >> 20) & 0xf : 0;
    PsEntry *ps = pixel_shader(fp_ea, texdim, remap, gamma);
    if (!vs || !ps->ps) { g_skip[5]++; return; }

    // constants
    lap(3);
    static CbSlice s_vc, s_vx, s_px, s_vch;   // static: a skipped upload (leak_mode 3) keeps the last binding
    const bool cb_on = leak_mode() != 3;
    // Vertex constants: only the windows the shader reads (VpInfo), re-sent when the
    // constants or the windows changed.
    static VsWin last_win{0xffff, 0, 0, 0};
    auto push_vc = [&] {
        s_vc = cb_push(&vp_consts[g_cur_win.lo_base * 4], g_cur_win.lo_count * 16);
        s_vch = g_cur_win.hi_count ? cb_push(&vp_consts[g_cur_win.hi_base * 4], g_cur_win.hi_count * 16) : s_vc;
        last_win = g_cur_win;
    };
    bool win_changed = memcmp(&last_win, &g_cur_win, sizeof last_win) != 0;
    if (!cb_on) {
    } else if (g_cbring) {
        if (vp_consts_dirty || win_changed || s_vc.gen != g_cbring_gen) push_vc();
    } else if (vp_consts_dirty) {
        upload_cb(g_cb_vc, vp_consts, sizeof vp_consts);
    }
    vp_consts_dirty = false;
    float vx[24 + 128] = {regf(0x0a30), regf(0x0a34), regf(0x0a38), regf(0x0a3c), regf(0x0a20), regf(0x0a24),
                    regf(0x0a28), regf(0x0a2c), (float)b.w, (float)b.h, 0, 0};
    // SET_USER_CLIP_PLANE_CONTROL (0x1478): 4 bits per plane, 1 = clip where d >= 0
    // (keep d < 0), 2 = clip where d < 0. Killzone 2 cuts mirrored geometry at the
    // mirror plane this way. KZ2_NO_CLIP_PLANES=1 ignores them.
    static const bool no_clip = getenv("KZ2_NO_CLIP_PLANES") && atoi(getenv("KZ2_NO_CLIP_PLANES"));
    for (int i = 0; i < 6; i++) {
        uint32_t m = no_clip ? 0 : (reg(0x1478) >> (4 * i)) & 0xf;
        vx[16 + (i < 3 ? 1 + i : 5 + (i - 3))] = m == 1 ? -1.f : m == 2 ? 1.f : 0.f;
    }
    memcpy(&vx[12], &base_vertex, 4);   // vbase.x: first vertex of this draw in the ring (uint)
    bool origin_bottom = (reg(0x1d88) >> 12) & 0xf;
    // Window-origin "bottom" (0x1d88 bit 12) maps to D3D top-down here; verified against
    // the font atlas, which must come out upright for text lookups to work.
    origin_bottom = !origin_bottom;
    static const bool flip_y = getenv("KZ2_FLIP_Y") != nullptr;
    if (flip_y) origin_bottom = !origin_bottom;
    vx[10] = origin_bottom ? 1.f : 0.f;
    memcpy(&vx[24], vattr, sizeof vattr);
    memcpy(&vx[24 + 64], g_vattr, sizeof g_vattr);
    if (!cb_on) {
    } else if (g_cbring) s_vx = cb_push(vx, sizeof vx);
    else upload_cb(g_cb_vx, vx, sizeof vx);
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
    // KZ2_WPOS_FLIP=1 (experiment): report WPOS with the opposite vertical origin from the
    // one used for rasterisation - tests position-based G-buffer lookups (light volumes).
    static const bool wpos_flip = !getenv("KZ2_WPOS_FLIP") || atoi(getenv("KZ2_WPOS_FLIP"));   // default on
    px.wpos[0] = (origin_bottom != wpos_flip) ? 1.f : 0.f;
    px.wpos[1] = (float)(reg(0x1d88) & 0xfff);
    px.wpos[2] = (float)g_aa_sx, px.wpos[3] = (float)g_aa_sy;
    for (size_t k = 0; k < ps->consts.size(); k++)
        for (int c = 0; c < 4; c++) px.fc[k * 4 + c] = fp_const_word(fp_ea + ps->consts[k] + 4 * c);
    if (!cb_on) {
    } else if (g_cbring) s_px = cb_push(&px, 32 + 256 + ps->consts.size() * 16);
    else upload_cb(g_cb_px, &px, 32 + 256 + ps->consts.size() * 16);
    if (cb_on && g_cbring && s_vx.gen != s_vc.gen) push_vc();   // ring wrapped in between
    if ((int)g_frame == g_log_frame) {
        float f[8];
        memcpy(f, px.fc, 32);
        uint64_t psh = 0;
        for (auto &[k, v] : g_ps) if (&v == ps) psh = k;
        if (!ps->dumped) {   // the translated program, for reading next to the log
            ps->dumped = true;
            std::filesystem::create_directories("shaders");
            char path[64];
            snprintf(path, sizeof path, "shaders/cap_ps_%016llx.hlsl", (unsigned long long)psh);
            if (FILE *f = fopen(path, "w")) { fwrite(ps->hlsl.data(), 1, ps->hlsl.size(), f); fclose(f); }
        }
        LOG("draw", "#%d rt=%x tex0=%x ps=%016llx cmask=%08x blend=%x/%x/%x fc0=(%g %g %g %g) fc1=(%g %g %g %g) n=%zu",
            g_draws_this_frame, b.color[0] ? b.color[0]->ea : 0, srv[0] ? ea_for((reg(0x1a04) & 3) == 2, reg(0x1a00)) : 0,
            (unsigned long long)psh, reg(0x324), reg(0x310), reg(0x314), reg(0x318), f[0], f[1], f[2], f[3], f[4], f[5], f[6], f[7], list.size());
        LOG("draw", "   fp control %08x (register now %08x), shader packer (sRGB out) %x", ps->control, reg(0x1d60), reg(0x1fc4));
        for (size_t k = 2; k < ps->consts.size(); k++) {   // fc0/fc1 are on the line above
            float v[4];
            memcpy(v, &px.fc[k * 4], 16);
            LOG("draw", "   fc%zu = (%g %g %g %g)", k, v[0], v[1], v[2], v[3]);
        }
        if (!g_vs_dumped[g_vs_hash]) {
            g_vs_dumped[g_vs_hash] = true;
            std::filesystem::create_directories("shaders");
            char path[64];
            snprintf(path, sizeof path, "shaders/cap_vs_%016llx.hlsl", (unsigned long long)g_vs_hash);
            const std::string &src = g_vs_hlsl[g_vs_hash];
            if (FILE *f = fopen(path, "w")) { fwrite(src.data(), 1, src.size(), f); fclose(f); }
        }
        LOG("draw", "   depth test %u func %x write %u, stencil %u func %x ref %x mask %x/%x ops %x/%x/%x two-sided %u, "
            "depth bounds %u [%g, %g]", reg(0x0a74) & 1, reg(0x0a6c), reg(0x0a70) & 1, reg(0x0328) & 1, reg(0x0330),
            reg(0x0334), reg(0x0338), reg(0x032c), reg(0x033c), reg(0x0340), reg(0x0344), reg(0x0348) & 1,
            reg(0x0380) & 1, regf(0x0384), regf(0x0388));
        LOG("draw", "   surface fmt=%08x %ux%u target=%x color=%x/%x/%x/%x depth=%x pitch=%x", reg(0x0208), b.w, b.h,
            reg(0x0220), b.color[0] ? b.color[0]->ea : 0, b.color[1] ? b.color[1]->ea : 0, b.color[2] ? b.color[2]->ea : 0,
            b.color[3] ? b.color[3]->ea : 0, b.depth ? b.depth->ea : 0, reg(0x020c));
        LOG("draw", "   zpitch %x, depth %s; blend MRT %x, colour mask MRT %08x", reg(0x022c), b.proxied[4] ? "via proxy" : "direct",
            reg(0x012c), reg(0x0370));
        {   // geometry: primitive, index buffer, vertex range, and each enabled attribute as fmt@offset
            std::string attrs;
            for (int a = 0; a < 16; a++)
                if (reg(0x1ff0) & (1 << a)) {
                    char s[40];
                    snprintf(s, sizeof s, " a%d=%08x@%08x", a, reg(0x1740 + a * 4), reg(0x1680 + a * 4));
                    attrs += s;
                }
            LOG("draw", "   geom vs=%016llx prim=%u indexed=%d index=%08x@%08x verts %u..%u base=%x/%x freq=%x:%s",
                (unsigned long long)g_vs_hash, g_prim, g_indexed, reg(0x1820), reg(0x181c), vmin, vmax, reg(0x1738),
                reg(0x173c), reg(0x1fc0), attrs.c_str());
            // Spike check on the decoded attribute-0 positions: share of triangles with an
            // edge over 8x the draw's median edge length.
            if (topo == D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST && list.size() >= 3) {
                std::vector<float> pos((size_t)nv * 3), edge;
                float vv[64];
                for (uint32_t i = 0; i < nv; i++) fetch_vertex(vv, vmin + i), memcpy(&pos[i * 3], vv, 12);
                auto len = [&](uint32_t a, uint32_t b) {
                    float d[3] = {pos[a * 3] - pos[b * 3], pos[a * 3 + 1] - pos[b * 3 + 1], pos[a * 3 + 2] - pos[b * 3 + 2]};
                    return std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
                };
                for (size_t i = 0; i + 2 < list.size(); i += 3)
                    edge.push_back(std::max({len(list[i], list[i + 1]), len(list[i + 1], list[i + 2]), len(list[i + 2], list[i])}));
                std::vector<float> sorted = edge;
                std::nth_element(sorted.begin(), sorted.begin() + sorted.size() / 2, sorted.end());
                float median = sorted[sorted.size() / 2];
                size_t spikes = 0;
                for (float e : edge) spikes += e > 8 * median;
                if (spikes * 50 > edge.size()) {
                    LOG("draw", "   SPIKES: %zu of %zu triangles have an edge > 8x the median (%g)", spikes, edge.size(), median);
                    for (size_t i = 0, shown = 0; i < edge.size() && shown < 3; i++) {
                        if (edge[i] <= 8 * median) continue;
                        shown++;
                        for (int k = 0; k < 3; k++) {
                            uint32_t v = list[i * 3 + k];
                            LOG("draw", "     tri %zu v%d: index %u (+%u) pos %g %g %g", i, k, v, vmin, pos[v * 3], pos[v * 3 + 1],
                                pos[v * 3 + 2]);
                        }
                    }
                }
            }
        }
        for (int t = 0; t < 16; t++)
            if (reg(0x1a0c + t * 32) & 0x80000000) {
                uint32_t tea = ea_for((reg(0x1a04 + t * 32) & 3) == 2, reg(0x1a00 + t * 32));
                auto rt = g_surfaces.find(tea);
                LOG("draw", "   tex%d ea=%x fmt=%08x rect=%08x pitch=%x filter=%08x addr=%08x remap=%04x ctl0=%08x %s", t, tea,
                    reg(0x1a04 + t * 32), reg(0x1a18 + t * 32), reg(0x1840 + t * 4) & 0xfffff, reg(0x1a14 + t * 32),
                    reg(0x1a08 + t * 32), reg(0x1a10 + t * 32) & 0xffff, reg(0x1a0c + t * 32),
                    rt != g_surfaces.end() ? ("(render target fmt " + std::to_string(rt->second.fmt) + " " +
                                              std::to_string(rt->second.w) + "x" + std::to_string(rt->second.h) + ")").c_str()
                                           : "");
                // Tiny textures (per-object data the SPUs write each frame, e.g. lighting
                // probes): log their raw words, and whether they are all zero.
                uint32_t tw = reg(0x1a18 + t * 32) >> 16, th = reg(0x1a18 + t * 32) & 0xffff;
                if (tw * th <= 256 && tw && th) {
                    std::string words;
                    bool zero = true;
                    for (uint32_t k = 0; k < tw * th; k++) {
                        uint32_t v = ld32(tea + 4 * k);
                        zero &= v == 0;
                        if (k < 16) { char s[12]; snprintf(s, sizeof s, " %08x", v); words += s; }
                    }
                    LOG("draw", "     tex%d data (%ux%u, %s):%s", t, tw, th, zero ? "ALL ZERO" : "non-zero", words.c_str());
                }
            }
        // Vertex shader and the first vertices' attributes: every full-screen pass, or KZ2_LOG_ATTRS=<draw>.
        static int attr_draw = getenv("KZ2_LOG_ATTRS") ? atoi(getenv("KZ2_LOG_ATTRS")) : -1;
        static int quad_logs;   // particle quads too (the first 40 of the frame)
        if (g_draw_logged_frame != (int)g_frame) g_draw_logged_frame = (int)g_frame, quad_logs = 0;
        if (g_draws_this_frame == attr_draw || list.size() <= 6 || (g_prim == 8 && quad_logs++ < 40)) {
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

    lap(4);
    apply_state();
    g_ctx->VSSetShader(vs, nullptr, 0);
    g_ctx->PSSetShader(ps->ps, nullptr, 0);
    ID3D11Buffer *vcb[2] = {g_cb_vc, g_cb_vx};
    if (g_cbring) {
        cb_flush();
        ID3D11Buffer *rings[3] = {g_cbring, g_cbring, g_cbring};
        UINT first[3] = {s_vc.first, s_vx.first, s_vch.first}, num[3] = {s_vc.num, s_vx.num, s_vch.num};
        g_ctx1->VSSetConstantBuffers1(0, 3, rings, first, num);
        g_ctx1->PSSetConstantBuffers1(0, 1, &g_cbring, &s_px.first, &s_px.num);
    } else {
        g_ctx->VSSetConstantBuffers(0, 2, vcb);
        g_ctx->PSSetConstantBuffers(0, 1, &g_cb_px);
    }
    g_ctx->VSSetShaderResources(0, 1, &g_vring.srv);
    // sampling a surface that is also being rendered to: use a snapshot
    for (auto &s : srv) {
        for (auto *c : b.color) if (s && c && s == c->srv) s = snapshot(*c);
        if (s && b.depth && s == b.depth->srv) s = snapshot(*b.depth);
    }
    g_ctx->PSSetShaderResources(0, 16, srv);
    g_ctx->PSSetSamplers(0, 16, smp);
    g_ctx->IASetIndexBuffer(g_iring.buf, DXGI_FORMAT_R32_UINT, 0);
    g_ctx->IASetPrimitiveTopology(topo);
    // Capture: full-screen passes (lighting, downsampling, post-processing) save every
    // input texture exactly as the shader receives it, and their outputs after the draw.
    // Also any draw sampling a large main-memory texture (e.g. the composite of the SPU
    // post-processing buffers, which is a mesh, not a quad).
    bool samples_main = false;
    for (int t = 0; t < 16; t++)
        if (srv[t] && ea_for((reg(0x1a04 + t * 32) & 3) == 2, reg(0x1a00 + t * 32)) < mem::LOCAL_BASE &&
            (reg(0x1a18 + t * 32) >> 16) * (reg(0x1a18 + t * 32) & 0xffff) >= 160 * 90)
            samples_main = true;
    // Skip draws that write no colour (occlusion queries); main-memory samplers always dump.
    const bool pass_dump = (int)g_frame == g_log_frame && reg(0x0324) &&
                           ((list.size() <= 6 && g_pass_dumps < 600) || samples_main);
    if (pass_dump)
        for (int t = 0; t < 16; t++)
            if (srv[t]) {
                char path[96];
                uint32_t tea = ea_for((reg(0x1a04 + t * 32) & 3) == 2, reg(0x1a00 + t * 32));
                snprintf(path, sizeof path, "pass_%04d_in%02d_%08x.bmp", g_draws_this_frame, t, tea);
                dump_srv(srv[t], path), g_pass_dumps++;
            }
    // Capture: shadowed lights (a shadow-compare sampler) save the light buffer before and
    // after the draw, and the shadow map, to see exactly what the light adds and where.
    bool shadow_light = false;
    for (int t = 0; t < 16; t++) if (srv[t] && (texdim[t] & TEX_SHADOW)) shadow_light = true;
    const bool light_dump = (int)g_frame == g_log_frame && shadow_light && b.color[0] && g_pass_dumps < 600;
    if (light_dump) {
        char path[96];
        snprintf(path, sizeof path, "light_%04d_before_%08x.bmp", g_draws_this_frame, b.color[0]->ea);
        dump_srv(b.color[0]->srv, path), g_pass_dumps++;
        for (int t = 0; t < 16; t++)
            if (srv[t] && (texdim[t] & TEX_SHADOW)) {
                uint32_t tea = ea_for((reg(0x1a04 + t * 32) & 3) == 2, reg(0x1a00 + t * 32));
                snprintf(path, sizeof path, "light_%04d_shadow%02d_%08x.bmp", g_draws_this_frame, t, tea);
                dump_srv(srv[t], path), g_pass_dumps++;
            }
    }
    lap(5);
    if (leak_mode() != 1) g_ctx->DrawIndexed((UINT)list.size(), start_index, 0);   // the shader adds vbase to SV_VertexID
    // Hand queued work to the GPU regularly, so it draws while we translate the next
    // commands: otherwise the driver submits only when a readback forces it, and the
    // CPU then waits for the GPU to do the whole frame's work at once.
    static const int flush_every = getenv("KZ2_FLUSH_EVERY") ? atoi(getenv("KZ2_FLUSH_EVERY")) : 256;
    static int since_flush;
    if (flush_every > 0 && ++since_flush >= flush_every) g_ctx->Flush(), since_flush = 0;
    lap(6);
    if (light_dump) {
        char path[96];
        snprintf(path, sizeof path, "light_%04d_after_%08x.bmp", g_draws_this_frame, b.color[0]->ea);
        dump_srv(b.color[0]->srv, path), g_pass_dumps++;
    }
    if (pass_dump)
        for (int k = 0; k < 4; k++)
            if (b.color[k]) {
                char path[96];
                snprintf(path, sizeof path, "pass_%04d_out%d_%08x.bmp", g_draws_this_frame, k, b.color[k]->ea);
                dump_srv(b.color[k]->srv, path), g_pass_dumps++;
            }
    // Only what the RSX would write is dirty: a zeta buffer stays bound while depth
    // and stencil are off, and depth-only passes mask colour writes.
    if (reg(0x0324)) for (int i = 0; i < 4; i++) if (b.color[i]) mark_drawn(*b.color[i], b.proxied[i]);
    // Early readback: a colour target in main memory is read by the SPUs (post-processing
    // inputs) once the game signals them. Start its GPU->CPU copy now and hand it to the
    // GPU, so the write-back at that signal finds it done instead of stalling the RSX
    // thread on the whole queue. KZ2_EARLY_READBACK=0: copy at write-back time.
    // Default OFF: the copy is taken right after a draw, but more draws (the half-res smoke
    // and particle passes) can follow before the SPUs read the buffer, so they read a stale
    // copy (blocky fog over the whole shore level). KZ2_EARLY_READBACK=1: on.
    static const bool early_rb = getenv("KZ2_EARLY_READBACK") && atoi(getenv("KZ2_EARLY_READBACK"));
    if (early_rb && reg(0x0324))
        for (int i = 0; i < 4; i++) {
            Surface *c = b.color[i];
            if (!c || b.proxied[i] || c->ea >= mem::LOCAL_BASE || c->depth) continue;
            if (!c->staging) {
                D3D11_TEXTURE2D_DESC d;
                c->tex->GetDesc(&d);
                d.Usage = D3D11_USAGE_STAGING, d.BindFlags = 0, d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
                if (FAILED(g_dev->CreateTexture2D(&d, nullptr, &c->staging))) continue;
            }
            g_ctx->CopyResource(c->staging, c->tex);
            c->copied_serial = c->drawn_serial;
            g_ctx->Flush();
        }
    bool zwrite = ((reg(0x0a74) & 1) && (reg(0x0a70) & 1)) || (reg(0x0328) & 1);
    if (b.depth && zwrite) mark_drawn(*b.depth, b.proxied[4]);
    // Capture: after each small draw (post-processing quads), save its target.
    // (Square targets are shadow maps: skipped.)
    // KZ2_STEP_EVERY=n with KZ2_STEP_RT=<ea>: also snapshot that target after every nth draw into it.
    static const int step_every = getenv("KZ2_STEP_EVERY") ? atoi(getenv("KZ2_STEP_EVERY")) : 0;
    static const uint32_t step_rt = getenv("KZ2_STEP_RT") ? (uint32_t)strtoul(getenv("KZ2_STEP_RT"), nullptr, 16) : 0;
    bool step = step_every > 0 && b.color[0] && b.color[0]->ea == step_rt && g_draws_this_frame % step_every == 0;
    if ((int)g_frame == g_log_frame && b.color[0] && g_step_dumps < (step_every ? 700 : 80) &&
        (step_every ? step : list.size() <= 96 && b.color[0]->w != b.color[0]->h)) {
        g_step_dumps++;
        char name[64];
        snprintf(name, sizeof name, "step_%04d_%08x", g_draws_this_frame, b.color[0]->ea);
        dump_surface(b.proxied[0] && b.color[0]->proxy ? *b.color[0]->proxy : *b.color[0], name);   // what was drawn
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

} // namespace
} // namespace rsx
// Hybrid-graphics drivers (NVIDIA Optimus, AMD PowerXpress) run executables exporting
// these on the discrete GPU.
extern "C" {
__declspec(dllexport) DWORD NvOptimusEnablement = 1;
__declspec(dllexport) int AmdPowerXpressRequestHighPerformance = 1;
}
namespace rsx {
namespace {

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
    // The default adapter is whatever Windows considers primary - often the integrated
    // GPU of an APU. Ask DXGI for the high-performance one (the discrete GPU).
    IDXGIAdapter1 *adapter = nullptr;
    IDXGIFactory6 *factory = nullptr;
    if (SUCCEEDED(CreateDXGIFactory1(__uuidof(IDXGIFactory6), (void **)&factory))) {
        factory->EnumAdapterByGpuPreference(0, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, __uuidof(IDXGIAdapter1),
                                            (void **)&adapter);
        factory->Release();
    }
    if (adapter) {
        DXGI_ADAPTER_DESC1 ad;
        adapter->GetDesc1(&ad);
        char name[128];
        WideCharToMultiByte(CP_UTF8, 0, ad.Description, -1, name, sizeof name, nullptr, nullptr);
        LOG("rsx", "GPU: %s (%llu MB video memory)", name, (unsigned long long)(ad.DedicatedVideoMemory >> 20));
    }
    HRESULT hr = D3D11CreateDeviceAndSwapChain(adapter, adapter ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE,
                                               nullptr, flags, &fl, 1, D3D11_SDK_VERSION, &sd, &g_swap, &g_dev, nullptr,
                                               &g_ctx);
    if (adapter) {
        adapter->QueryInterface(__uuidof(IDXGIAdapter3), (void **)&g_adapter3);
        adapter->Release();
    }
    if (FAILED(hr)) { LOG("rsx", "D3D11 device creation failed (0x%08lx)", hr); return false; }
    D3D11_FEATURE_DATA_D3D11_OPTIONS opts{};
    g_ring_ok = SUCCEEDED(g_dev->CheckFeatureSupport(D3D11_FEATURE_D3D11_OPTIONS, &opts, sizeof opts)) &&
                opts.MapNoOverwriteOnDynamicBufferSRV;
    if (!g_ring_ok) LOG("rsx", "driver lacks MapNoOverwriteOnDynamicBufferSRV: per-draw buffers are discarded (slow)");
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
    static const char DEPTH_X16_HLSL[] =
        "Texture2D<float> d : register(t0);" "\n"
        "float4 ps(float4 p : SV_Position) : SV_Target { return d.Load(int3(p.xy, 0)).xxxx; }" "\n";
    if (SUCCEEDED(D3DCompile(DEPTH_X16_HLSL, sizeof DEPTH_X16_HLSL - 1, nullptr, nullptr, nullptr, "ps", "ps_5_0", 0, 0,
                             &code, &err))) {
        g_dev->CreatePixelShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &g_depth_x16_ps);
        code->Release();
    }
    static const char DEPTH_COPY_HLSL[] =
        "Texture2D<float> d : register(t0);\n"
        "float ps(float4 p : SV_Position) : SV_Depth { return d.Load(int3(p.xy, 0)); }\n";
    if (FAILED(D3DCompile(DEPTH_COPY_HLSL, sizeof DEPTH_COPY_HLSL - 1, nullptr, nullptr, nullptr, "ps", "ps_5_0", 0, 0,
                          &code, &err))) {
        LOG("rsx", "depth copy shader: %s", err ? (const char *)err->GetBufferPointer() : "?");
    } else {
        g_dev->CreatePixelShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &g_depth_copy_ps);
        code->Release();
    }
    static const char CUBE_FACE_HLSL[] =
        "TextureCube c : register(t0); SamplerState s : register(s0);" "\n"
        "cbuffer P : register(b0) { uint4 mode; };" "\n"
        "float4 ps(float4 p : SV_Position) : SV_Target { float2 q = p.xy / 128 * 2 - 1; float3 d;" "\n"
        "  switch (mode.x) { case 0: d = float3(1, -q.y, -q.x); break; case 1: d = float3(-1, -q.y, q.x); break;" "\n"
        "  case 2: d = float3(q.x, 1, q.y); break; case 3: d = float3(q.x, -1, -q.y); break;" "\n"
        "  case 4: d = float3(q.x, -q.y, 1); break; default: d = float3(-q.x, -q.y, -1); break; }" "\n"
        "  return float4(c.SampleLevel(s, d, 0).rgb, 1); }" "\n";
    if (SUCCEEDED(D3DCompile(CUBE_FACE_HLSL, sizeof CUBE_FACE_HLSL - 1, nullptr, nullptr, nullptr, "ps", "ps_5_0", 0, 0,
                             &code, &err))) {
        g_dev->CreatePixelShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &g_cube_face_ps);
        code->Release();
    }
    D3D11_DEPTH_STENCIL_DESC dsd{};
    dsd.DepthEnable = TRUE, dsd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL, dsd.DepthFunc = D3D11_COMPARISON_ALWAYS;
    g_dev->CreateDepthStencilState(&dsd, &g_ds_always);
    D3D11_SAMPLER_DESC smp{};
    smp.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    smp.AddressU = smp.AddressV = smp.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    smp.MaxLOD = D3D11_FLOAT32_MAX;
    g_dev->CreateSamplerState(&smp, &g_present_smp);
    g_cb_vc = make_cb(sizeof vp_consts);
    // Constant ring (see cb_push), when the driver supports offsets and NO_OVERWRITE on constant buffers.
    if (opts.ConstantBufferOffsetting && opts.MapNoOverwriteOnDynamicConstantBuffer &&
        SUCCEEDED(g_ctx->QueryInterface(__uuidof(ID3D11DeviceContext1), (void **)&g_ctx1))) {
        g_cbring = make_cb(CBRING_SIZE);
        // constant windows need buffer offsets (D3D11.1). KZ2_VS_WINDOWS=0: whole constant file.
        g_vp_windows = g_cbring != nullptr && (!getenv("KZ2_VS_WINDOWS") || atoi(getenv("KZ2_VS_WINDOWS")));
        if (!g_cbring) release(g_ctx1);
    }
    if (!g_cbring) LOG("rsx", "driver lacks constant buffer offsets: constants are re-uploaded per draw");
    g_cb_vx = make_cb(sizeof(float) * (24 + 128));
    g_cb_px = make_cb(32 + 256 + 128 * 16);
    g_cb_present = make_cb(16);
    g_cb_unit_uv = make_cb(16);
    { float one[4] = {1, 1, 0, 0}; upload_cb(g_cb_unit_uv, one, sizeof one); }
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
        for (int i = 0; i < 4; i++)
            if (b.color[i]) g_ctx->ClearRenderTargetView(b.rtv[i], f), mark_drawn(*b.color[i], b.proxied[i]);
    }
    if ((mask & 3) && b.depth) {
        uint32_t z = reg(0x1d8c);
        UINT fl = (mask & 1 ? D3D11_CLEAR_DEPTH : 0) | (mask & 2 ? D3D11_CLEAR_STENCIL : 0);
        float depth = b.depth->fmt == 1 ? (z & 0xffff) / 65535.f : (z >> 8) / 16777215.f;
        g_ctx->ClearDepthStencilView(b.dsv, fl, depth, (UINT8)(z & 0xff));
        mark_drawn(*b.depth, b.proxied[4]);
    }
    if ((int)g_frame == g_log_frame)
        LOG("draw", "clear mask %x (after draw #%d): %ux%u color=%x depth=%x%s (surface %ux%u, zpitch %x) z=%08x color=%08x",
            mask, g_draws_this_frame, b.w, b.h, b.color[0] ? b.color[0]->ea : 0, b.depth ? b.depth->ea : 0,
            b.proxied[4] ? " via proxy" : "", b.depth ? b.depth->w : 0, b.depth ? b.depth->h : 0, reg(0x022c), reg(0x1d8c),
            reg(0x1d90));
}

void begin_end(uint32_t prim) {
    if (prim) {
        g_prim = prim;
        g_ranges.clear();
        g_indexed = false;
    } else {
        auto t0 = std::chrono::steady_clock::now();
        draw();
        g_draw_us += std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0).count();
        g_draw_calls++;
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
        dump_surface(s, name, true);   // alpha channel (specular etc. often live there)
        // Also as the game reads its packed G-buffer target: two half floats per pixel
        // (Y16_X16_FLOAT, e.g. view-space normal X/Y), mapped -1..1 -> 0..255 in red/green.
        if (color_format(s.fmt) == DXGI_FORMAT_R8G8B8A8_UNORM && s.w >= 640)
            if (ID3D11ShaderResourceView *v = reinterpret_view(s, 0x9f)) {
                char path[48];
                snprintf(path, sizeof path, "rt_%08x_as_y16x16f.bmp", ea);
                dump_srv(v, path, true);
            }
    }
    LOG("rsx", "dumped %zu surfaces at frame %u", g_surfaces.size(), g_frame);
}

void request_capture() { g_capture_req = true; }
void toggle_cube_maps() { g_no_cube = !g_no_cube; LOG("rsx", "F11: cube maps %s", g_no_cube ? "off (black)" : "on"); }
bool capturing() {
    return g_capture_req || (g_log_frame >= 0 && (int)g_frame >= g_log_frame && (int)g_frame <= g_dump_frame);
}

// Copy colour surfaces rendered since the last sync point back to guest
// memory in RSX layout. Called when the RSX signals progress (labels,
// semaphores, user commands, flips): from then on the PPU and SPUs may read
// what it rendered - Killzone 2 post-processes downsampled frames on the SPUs.
// Only surfaces in main memory are copied here: those are what the PPU and SPUs
// read. Surfaces in RSX local memory stay on the GPU until a texture or NV0039
// copy needs their bytes (write_back_surface on demand); each copy stalls on
// the GPU. KZ2_WB_LOCAL=1 also copies local surfaces at every sync point.
void write_back(bool all) {
    if (leak_mode() == 6) return;
    static const bool wb_local = getenv("KZ2_WB_LOCAL") && atoi(getenv("KZ2_WB_LOCAL"));
    all |= wb_local;
    auto t0 = std::chrono::steady_clock::now();
    for (auto &[ea, s] : g_surfaces) if (all || ea < mem::LOCAL_BASE) write_back_surface(ea, s);
    for (auto &[ea, s] : g_depth_surfaces) if (all || ea < mem::LOCAL_BASE) write_back_surface(ea, s);
    g_wb_us += std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0).count();
    g_wb_calls++;
}
std::string stats_line(uint32_t frames) {
    char b[600];
    double f = frames;
    PROCESS_MEMORY_COUNTERS_EX pmc{};
    K32GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS *)&pmc, sizeof pmc);
    DXGI_QUERY_VIDEO_MEMORY_INFO vram{}, sysram{};
    if (g_adapter3) {
        g_adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &vram);
        g_adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL, &sysram);
    }
    snprintf(b, sizeof b,
             "RSX per frame: draws %.0f ms (%.0f), write-back %.0f ms (%.0f); surface readbacks %.0f ms (%.0f), "
             "texture uploads %.0f ms (%.0f), shader compiles %.0f ms (%.0f), vertex fetch %.0f ms (%.1fM vertices "
             "for %.1fM indices; Map+copy %.0f ms), draw calls %.0f ms; present %.0f ms; "
             "memory: %zu textures, %zu surfaces, process %llu MB in use (%llu MB private, %llu MB of it guest memory), "
             "GPU %llu MB in video memory + %llu MB in system memory",
             g_draw_us / 1e3 / f, g_draw_calls / f, g_wb_us / 1e3 / f, g_wb_calls / f, g_wbs_us / 1e3 / f, g_wbs_n / f,
             g_tex_us / 1e3 / f, g_tex_n / f, g_compile_us / 1e3 / f, g_compile_n / f, g_fetch_us / 1e3 / f,
             g_fetch_verts / 1e6 / f, g_fetch_used / 1e6 / f, g_map_us / 1e3 / f, g_dcall_us / 1e3 / f,
             g_present_us / 1e3 / f, g_textures.size(), g_surfaces.size() + g_depth_surfaces.size(),
             (unsigned long long)(pmc.WorkingSetSize >> 20), (unsigned long long)(pmc.PrivateUsage >> 20),
             (unsigned long long)(mem::committed() >> 20),
             (unsigned long long)(vram.CurrentUsage >> 20), (unsigned long long)(sysram.CurrentUsage >> 20));
    g_wb_us = g_wb_calls = g_draw_us = g_draw_calls = g_present_us = 0;
    g_wbs_us = g_wbs_n = g_tex_us = g_tex_n = g_compile_us = g_compile_n = 0;
    g_fetch_us = g_fetch_verts = g_fetch_used = g_dcall_us = g_map_us = 0;
    char seg[200];
    snprintf(seg, sizeof seg, "; draw stages ms/frame: bind %.1f, geometry %.1f, vs+textures %.1f, ps %.1f, "
             "constants %.1f, state+binds %.1f, DrawIndexed %.1f (of vs+textures: vertex shader %.1f, texture hashing %.1f (%.0f))",
             g_seg_ns[0] / 1e6 / f, g_seg_ns[1] / 1e6 / f, (g_seg_ns[2] + g_seg_ns[7]) / 1e6 / f, g_seg_ns[3] / 1e6 / f,
             g_seg_ns[4] / 1e6 / f, g_seg_ns[5] / 1e6 / f, g_seg_ns[6] / 1e6 / f, g_seg_ns[7] / 1e6 / f,
             g_hash_ns / 1e6 / f, g_hash_n / f);
    g_hash_ns = g_hash_n = 0;
    for (auto &x : g_seg_ns) x = 0;
    char sk[200];
    snprintf(sk, sizeof sk,
             "; draws skipped per frame: no ranges %.0f, no target %.0f, bad range %.0f, ring full %.0f, no primitives "
             "%.0f, no shader %.0f",
             g_skip[0] / f, g_skip[1] / f, g_skip[2] / f, g_skip[3] / f, g_skip[4] / f, g_skip[5] / f);
    memset(g_skip, 0, sizeof g_skip);
    char sc[120] = "";
    if (g_scene_frames)
        snprintf(sc, sizeof sc, "; scene draws per frame min %d avg %.0f max %d", g_scene_min,
                 (double)g_scene_sum / g_scene_frames, g_scene_max);
    g_scene_min = 1 << 30, g_scene_max = 0, g_scene_sum = 0, g_scene_frames = 0;
    return std::string(b) + seg + sk + sc;
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
    extern void frame_delay_rsx();
    frame_delay_rsx();
    cb_flush();
    g_frame++;
    if ((int)g_frame == g_dump_frame) dump_surfaces();
    if (g_capture_req.exchange(false)) {
        g_log_frame = (int)g_frame, g_dump_frame = (int)g_frame + 1, g_step_dumps = 0, g_pass_dumps = 0;
        // Each capture replaces the last: delete the previous images (one capture
        // writes hundreds of MB) and translated shaders, so only this frame's remain.
        std::error_code ec;
        for (auto &e : std::filesystem::directory_iterator(".", ec))
            if (e.path().extension() == ".bmp" || (e.path().extension() == ".bin" && e.path().filename().string().rfind("tex_", 0) == 0))
                std::filesystem::remove(e.path(), ec);
        for (auto &e : std::filesystem::directory_iterator("shaders", ec))
            if (e.path().filename().string().rfind("cap_", 0) == 0) std::filesystem::remove(e.path(), ec);
        for (auto &[k, v] : g_ps) v.dumped = false;
        g_vs_dumped.clear();
        LOG("rsx", "capture: logging draws of frame %u, dumping its surfaces at the end", g_frame);
    }
    static const bool trace_draws = getenv("KZ2_TRACE_DRAWS") != nullptr;
    if (trace_draws && g_frame % 60 == 0) LOG("rsx", "frame %u: %d draws", g_frame, g_draws_this_frame);
    g_draws_this_frame = 0;
    if (g_scene_draws) {
        g_scene_min = std::min(g_scene_min, g_scene_draws), g_scene_max = std::max(g_scene_max, g_scene_draws);
        g_scene_sum += g_scene_draws, g_scene_frames++;
    }
    g_scene_draws = 0;
    // Streamed textures come and go at new addresses: once a second, release
    // those unused for 10 s (time, not frames: at low frame rates 600 frames is minutes).
    static const auto t_start = std::chrono::steady_clock::now();
    g_flip_time = std::chrono::duration<double>(std::chrono::steady_clock::now() - t_start).count();
    if (getenv("KZ2_LEAK_BISECT")) {
        static double last_log = 0;
        if (g_flip_time - last_log >= 1) {
            last_log = g_flip_time;
            PROCESS_MEMORY_COUNTERS_EX pmc{};
            K32GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS *)&pmc, sizeof pmc);
            LOG("leak", "t=%.0f s mode %d: %llu MB private", g_flip_time, leak_mode(),
                (unsigned long long)(pmc.PrivateUsage >> 20));
        }
    }
    static double last_sweep = 0;
    if (g_flip_time - last_sweep >= 1) {
        last_sweep = g_flip_time;
        for (auto it = g_textures.begin(); it != g_textures.end();) {
            if (g_flip_time - it->second.last_used > 10) {
                release(it->second.srv);
                release(it->second.tex);
                it = g_textures.erase(it);
            } else {
                ++it;
            }
        }
    }
    uint32_t ea = local_ea(offset);
    ID3D11ShaderResourceView *src = nullptr;
    float uv[4] = {1, 1, 0, 0};
    auto it = g_surfaces.find(ea);
    if (it != g_surfaces.end() && !it->second.depth) {
        sync_primary(it->second);
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
    auto t0 = std::chrono::steady_clock::now();
    occ_poll(true);   // every occlusion report of the frame is written before the game sees the flip
    {
        static int n;
        if (g_occ_reports && n++ < 10) LOG("rsx", "occlusion reports this frame: %llu (%llu visible)",
                                            (unsigned long long)g_occ_reports, (unsigned long long)g_occ_visible);
        g_occ_reports = g_occ_visible = 0;
    }
    extern bool pc_vsync();        // GRAPHICS menu (pc_options.cpp)
    g_swap->Present(pc_vsync() ? 1 : 0, 0);
    g_present_us += std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0).count();
}

void write_back(bool all);
void copy_m2mf() {
    // NV0039: line-by-line copy between local and main memory. The source may
    // be a surface rendered earlier in the stream: write it back first.
    write_back(true);
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
    if (m == 0x006c || m == 0x1d70 || m == 0x1d74 || m == 0xeb00 || m == 0xfeac) write_back(false);
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
    case 0x1d84:     // SET_ZPASS_PIXEL_COUNT_ENABLE
        if (g_ok) { g_occ_enabled = arg != 0; if (g_occ_enabled) occ_begin(); else occ_end_active(); }
        break;
    case 0x17c8:     // CLEAR_REPORT_VALUE (1: ZPASS_PIXEL_CNT)
        if (g_ok && arg == 1) occ_clear();
        break;
    case 0x1e98: {   // SET_RENDER_ENABLE: mode << 24 | report offset; 1: always draw, 2: only if the count != 0
        uint32_t mode = arg >> 24, off = arg & 0xffffff, ctx = reg(0x01a8);
        static int logs;
        if (logs++ < 8) LOG("rsx", "SET_RENDER_ENABLE mode %u offset 0x%x", mode, off);
        if (mode != 2) { g_render_enabled = true; break; }
        uint32_t ea = ctx == 0x66626660 ? mem::LOCAL_BASE + 0x0e000000 + off : io_ea(off);
        if (!ea) { g_render_enabled = true; break; }
        occ_poll(true);   // the report this depends on must be known now
        g_render_enabled = ld32(ea + 8) != 0;
        break;
    }
    case 0x1800: {   // GET_REPORT: arg = type << 24 | offset; writes {u64 timer, u32 value, u32 0}
        // Occlusion queries (type 1, ZPASS_PIXEL_CNT) are answered "visible" for now
        // (KZ2_REPORT_ZERO=1 answers 0 instead): unanswered, the game culled objects and
        // lights from whatever stale memory held. Report area: RSX local memory
        // + 0x0e000000 (context 0x66626660) or io-mapped main memory.
        uint32_t type = arg >> 24, off = arg & 0xffffff, ctx = reg(0x01a8);
        uint32_t ea = ctx == 0x66626660 ? mem::LOCAL_BASE + 0x0e000000 + off : io_ea(off);
        static int logs;
        if (logs++ < 24) LOG("rsx", "GET_REPORT type %u offset 0x%x (dma ctx 0x%08x) -> ea 0x%x", type, off, ctx, ea);
        if (!ea) break;
        static const bool zero = getenv("KZ2_REPORT_ZERO") != nullptr;
        static const bool visible = getenv("KZ2_REPORT_VISIBLE") && atoi(getenv("KZ2_REPORT_VISIBLE"));
        if (type == 1 && g_ok && !zero && !visible) {
            occ_end_active();   // close the current stretch; counting continues in a new one
            PendingReport r{ea, g_occ_since_clear};
            for (ID3D11Query *q : r.queries) g_query_refs[q]++;
            if (r.queries.empty()) write_report(ea, 0);
            else g_reports.push_back(std::move(r));
            occ_begin();
            break;
        }
        uint32_t value = type == 1 ? (zero ? 0 : 0x100000) : 0;
        uint64_t t = (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(
                         std::chrono::steady_clock::now().time_since_epoch()).count();
        st32(ea, (uint32_t)(t >> 32)), st32(ea + 4, (uint32_t)t), st32(ea + 8, value), st32(ea + 12, 0);
        break;
    }
    case 0x2328: copy_m2mf(); break;
    default:
        if (m >= 0x4000 && m != 0xfeac) {
            static bool seen[0x4000];
            if (!seen[m >> 2]) { seen[m >> 2] = true; LOG("rsx", "unhandled 2D-engine method 0x%04x = 0x%08x", m, arg); }
        }
    }
}
} // namespace rsx
