// The graphics API under the RSX renderer (d3d.cpp), so it can run on Direct3D 11 or Direct3D 12.
//
// It offers what the renderer uses, shaped like the part of Direct3D 11 it was written against:
// the same calls and the same description structs and enums (D3D11_TEXTURE2D_DESC, D3D11_BLEND_DESC,
// D3D11_MAP, ...), which are plain data here. Only the objects are ours. gpu_d3d11.cpp passes
// everything to Direct3D 11; gpu_d3d12.cpp implements it on Direct3D 12.
//
// Objects are reference counted like COM objects: created with one reference, Release() drops one,
// and calls that hand out an existing object (OMGetRenderTargets, GetResource) add one.
#pragma once

#include <windows.h>
#include <d3d11_1.h>
#include <dxgi1_6.h>

#include <atomic>
#include <string>

namespace gpu
{
    struct Object
    {
        virtual ~Object() = default;
        ULONG AddRef() { return ++refs; }
        ULONG Release()
        {
            const ULONG n = --refs;
            if (!n)
                delete this;
            return n;
        }

    private:
        std::atomic<ULONG> refs{1};
    };

    struct Resource : Object
    {
        enum Kind { TEXTURE2D, BUFFER } kind;
        explicit Resource(Kind k) : kind(k) {}
    };
    struct Texture2D : Resource
    {
        D3D11_TEXTURE2D_DESC desc{};
        Texture2D() : Resource(TEXTURE2D) {}
        void GetDesc(D3D11_TEXTURE2D_DESC *d) const { *d = desc; }
    };
    struct Buffer : Resource
    {
        D3D11_BUFFER_DESC desc{};
        Buffer() : Resource(BUFFER) {}
    };
    // The texture behind a resource, or null.
    inline Texture2D *as_texture(Resource *r) { return r && r->kind == Resource::TEXTURE2D ? static_cast<Texture2D *>(r) : nullptr; }

    struct View : Object
    {
        Resource *resource = nullptr;   // holds a reference
        ~View() override
        {
            if (resource)
                resource->Release();
        }
        void GetResource(Resource **r) const
        {
            resource->AddRef();
            *r = resource;
        }
    };
    struct SRV : View
    {
        D3D11_SHADER_RESOURCE_VIEW_DESC desc{};
        void GetDesc(D3D11_SHADER_RESOURCE_VIEW_DESC *d) const { *d = desc; }
    };
    struct RTV : View
    {
        DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    };
    struct DSV : View
    {
        DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    };

    struct VertexShader : Object {};
    struct PixelShader : Object {};
    struct BlendState : Object { D3D11_BLEND_DESC desc{}; };
    struct DepthStencilState : Object { D3D11_DEPTH_STENCIL_DESC desc{}; };
    struct RasterizerState : Object { D3D11_RASTERIZER_DESC desc{}; };
    struct SamplerState : Object { D3D11_SAMPLER_DESC desc{}; };
    struct Query : Object { D3D11_QUERY_DESC desc{}; };

    // Temporal upscaling (DX12: AMD FidelityFX Super Resolution). Inputs at render size, output at
    // display size; the output texture needs D3D11_BIND_UNORDERED_ACCESS.
    struct UpscaleParams
    {
        Texture2D *color = nullptr, *depth = nullptr, *motion = nullptr, *output = nullptr;
        UINT render_w = 0, render_h = 0, out_w = 0, out_h = 0;
        float jitter_x = 0, jitter_y = 0;          // the camera's sub-pixel offset this frame, in render pixels
        float mv_scale_x = 1, mv_scale_y = 1;      // motion vectors * scale = render pixels
        float frame_ms = 16.7f, near_z = 0.1f, far_z = 1000.f, fov_y = 1.f;
        float sharpness = 0;                       // 0: no extra sharpening
        bool reset = false;                        // the camera jumped: no history
        bool inverted_depth = false;
        bool infinite_depth = false;               // the projection has no far plane
    };

    struct Device
    {
        virtual ~Device() = default;
        virtual HRESULT CreateTexture2D(const D3D11_TEXTURE2D_DESC *desc, const D3D11_SUBRESOURCE_DATA *init, Texture2D **out) = 0;
        virtual HRESULT CreateBuffer(const D3D11_BUFFER_DESC *desc, const D3D11_SUBRESOURCE_DATA *init, Buffer **out) = 0;
        virtual HRESULT CreateShaderResourceView(Resource *r, const D3D11_SHADER_RESOURCE_VIEW_DESC *desc, SRV **out) = 0;
        virtual HRESULT CreateRenderTargetView(Resource *r, const D3D11_RENDER_TARGET_VIEW_DESC *desc, RTV **out) = 0;
        virtual HRESULT CreateDepthStencilView(Resource *r, const D3D11_DEPTH_STENCIL_VIEW_DESC *desc, DSV **out) = 0;
        virtual HRESULT CreateVertexShader(const void *code, SIZE_T size, void *linkage, VertexShader **out) = 0;
        virtual HRESULT CreatePixelShader(const void *code, SIZE_T size, void *linkage, PixelShader **out) = 0;
        virtual HRESULT CreateBlendState(const D3D11_BLEND_DESC *desc, BlendState **out) = 0;
        virtual HRESULT CreateDepthStencilState(const D3D11_DEPTH_STENCIL_DESC *desc, DepthStencilState **out) = 0;
        virtual HRESULT CreateRasterizerState(const D3D11_RASTERIZER_DESC *desc, RasterizerState **out) = 0;
        virtual HRESULT CreateSamplerState(const D3D11_SAMPLER_DESC *desc, SamplerState **out) = 0;
        virtual HRESULT CreateQuery(const D3D11_QUERY_DESC *desc, Query **out) = 0;
        // Dynamic structured buffers may be mapped NO_OVERWRITE (the vertex ring relies on it).
        virtual bool NoOverwriteOnBufferSRV() = 0;
        // VSSetConstantBuffers1 / PSSetConstantBuffers1 (constant buffers bound at an offset) work.
        virtual bool ConstantBufferOffsets() = 0;
        virtual const char *Name() = 0;   // "DX11", "DX12"
        // Backend costs for the fps log, per frame over `frames` frames (resets them).
        virtual std::string Stats(uint32_t frames) { return {}; }
        // The upscaler this device offers ("FSR 4.0.3"), or empty: none.
        virtual std::string UpscalerName() { return {}; }
    };

    struct Context
    {
        virtual ~Context() = default;
        virtual HRESULT Map(Resource *r, UINT sub, D3D11_MAP type, UINT flags, D3D11_MAPPED_SUBRESOURCE *out) = 0;
        virtual void Unmap(Resource *r, UINT sub) = 0;
        virtual void UpdateSubresource(Resource *r, UINT sub, const D3D11_BOX *box, const void *data, UINT row_pitch, UINT depth_pitch) = 0;
        virtual void CopyResource(Resource *dst, Resource *src) = 0;
        virtual void CopySubresourceRegion(Resource *dst, UINT dst_sub, UINT x, UINT y, UINT z, Resource *src, UINT src_sub,
                                           const D3D11_BOX *box) = 0;

        virtual void VSSetShader(VertexShader *vs, void *classes = nullptr, UINT n = 0) = 0;
        virtual void PSSetShader(PixelShader *ps, void *classes = nullptr, UINT n = 0) = 0;
        virtual void VSSetShaderResources(UINT start, UINT n, SRV *const *views) = 0;
        virtual void PSSetShaderResources(UINT start, UINT n, SRV *const *views) = 0;
        virtual void PSSetSamplers(UINT start, UINT n, SamplerState *const *samplers) = 0;
        virtual void VSSetConstantBuffers(UINT start, UINT n, Buffer *const *buffers) = 0;
        virtual void PSSetConstantBuffers(UINT start, UINT n, Buffer *const *buffers) = 0;
        // Offsets and sizes in 16-byte constants, multiples of 16 (as in Direct3D 11.1).
        virtual void VSSetConstantBuffers1(UINT start, UINT n, Buffer *const *buffers, const UINT *first, const UINT *num) = 0;
        virtual void PSSetConstantBuffers1(UINT start, UINT n, Buffer *const *buffers, const UINT *first, const UINT *num) = 0;

        virtual void IASetIndexBuffer(Buffer *b, DXGI_FORMAT format, UINT offset) = 0;
        virtual void IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY topology) = 0;
        virtual void RSSetState(RasterizerState *s) = 0;
        virtual void RSSetViewports(UINT n, const D3D11_VIEWPORT *vp) = 0;
        virtual void RSGetViewports(UINT *n, D3D11_VIEWPORT *vp) = 0;
        virtual void RSSetScissorRects(UINT n, const D3D11_RECT *rects) = 0;
        virtual void OMSetRenderTargets(UINT n, RTV *const *rtvs, DSV *dsv) = 0;
        virtual void OMGetRenderTargets(UINT n, RTV **rtvs, DSV **dsv) = 0;   // adds a reference to each
        virtual void OMSetBlendState(BlendState *s, const FLOAT factor[4], UINT sample_mask) = 0;
        virtual void OMSetDepthStencilState(DepthStencilState *s, UINT stencil_ref) = 0;

        virtual void Draw(UINT count, UINT start) = 0;
        virtual void DrawIndexed(UINT count, UINT start, INT base_vertex) = 0;
        virtual void ClearRenderTargetView(RTV *v, const FLOAT color[4]) = 0;
        virtual void ClearDepthStencilView(DSV *v, UINT flags, FLOAT depth, UINT8 stencil) = 0;

        virtual void Begin(Query *q) = 0;
        virtual void End(Query *q) = 0;
        virtual HRESULT GetData(Query *q, void *data, UINT size, UINT flags) = 0;
        virtual void Flush() = 0;
        // Upscales p.color into p.output; false: not available (bindings are unchanged either way).
        virtual bool Upscale(const UpscaleParams &p) { return false; }
    };

    // The window's swapchain.
    struct Swap
    {
        virtual ~Swap() = default;
        virtual RTV *Backbuffer() = 0;              // this frame's buffer (no reference added)
        virtual void Resize(UINT w, UINT h) = 0;    // render targets must be unbound first
        virtual void Present(bool vsync) = 0;
    };

    // Creates the device, its context and the window's swapchain on the given adapter.
    bool create_d3d11(IDXGIAdapter1 *adapter, HWND hwnd, bool debug, Device **dev, Context **ctx, Swap **swap);
    bool create_d3d12(IDXGIAdapter1 *adapter, HWND hwnd, bool debug, Device **dev, Context **ctx, Swap **swap);
} // namespace gpu
