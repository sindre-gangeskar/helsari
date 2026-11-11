// gpu.h on Direct3D 11: every call goes straight to the D3D11 device and immediate context.
#include "gpu.h"
#include "runtime.h"

namespace gpu
{
    namespace
    {
        template <class T> void release(T *&p)
        {
            if (p)
                p->Release(), p = nullptr;
        }

        // Our objects, each holding its D3D11 object.
        template <class Base, class Native> struct Wrap : Base
        {
            Native *p = nullptr;
            ~Wrap() override { release(p); }
        };
        using Tex11 = Wrap<Texture2D, ID3D11Texture2D>;
        using Buf11 = Wrap<Buffer, ID3D11Buffer>;
        using SRV11 = Wrap<SRV, ID3D11ShaderResourceView>;
        using RTV11 = Wrap<RTV, ID3D11RenderTargetView>;
        using DSV11 = Wrap<DSV, ID3D11DepthStencilView>;
        using VS11 = Wrap<VertexShader, ID3D11VertexShader>;
        using PS11 = Wrap<PixelShader, ID3D11PixelShader>;
        using Blend11 = Wrap<BlendState, ID3D11BlendState>;
        using Depth11 = Wrap<DepthStencilState, ID3D11DepthStencilState>;
        using Raster11 = Wrap<RasterizerState, ID3D11RasterizerState>;
        using Sampler11 = Wrap<SamplerState, ID3D11SamplerState>;
        using Query11 = Wrap<Query, ID3D11Query>;

        ID3D11Resource *native(Resource *r)
        {
            if (!r)
                return nullptr;
            return r->kind == Resource::TEXTURE2D ? (ID3D11Resource *)static_cast<Tex11 *>(r)->p : static_cast<Buf11 *>(r)->p;
        }
        template <class W, class T> auto nat(T *o) { return o ? static_cast<W *>(o)->p : nullptr; }

        struct Device11 final : Device
        {
            ID3D11Device *d = nullptr;
            bool no_overwrite_srv = false, cb_offsets = false;

            HRESULT CreateTexture2D(const D3D11_TEXTURE2D_DESC *desc, const D3D11_SUBRESOURCE_DATA *init, Texture2D **out) override
            {
                auto *t = new Tex11;
                HRESULT hr = d->CreateTexture2D(desc, init, &t->p);
                if (FAILED(hr))
                    return delete t, *out = nullptr, hr;
                t->desc = *desc;
                *out = t;
                return hr;
            }
            HRESULT CreateBuffer(const D3D11_BUFFER_DESC *desc, const D3D11_SUBRESOURCE_DATA *init, Buffer **out) override
            {
                auto *b = new Buf11;
                HRESULT hr = d->CreateBuffer(desc, init, &b->p);
                if (FAILED(hr))
                    return delete b, *out = nullptr, hr;
                b->desc = *desc;
                *out = b;
                return hr;
            }
            template <class W, class V, class Desc, class Fn> HRESULT view(Resource *r, const Desc *desc, V **out, Fn create)
            {
                auto *v = new W;
                HRESULT hr = create(native(r), desc, &v->p);
                if (FAILED(hr))
                    return delete v, *out = nullptr, hr;
                r->AddRef();
                v->resource = r;
                *out = v;
                return hr;
            }
            HRESULT CreateShaderResourceView(Resource *r, const D3D11_SHADER_RESOURCE_VIEW_DESC *desc, SRV **out) override
            {
                HRESULT hr = view<SRV11>(r, desc, out, [&](ID3D11Resource *n, auto *dd, auto **p) { return d->CreateShaderResourceView(n, dd, p); });
                if (SUCCEEDED(hr))
                    static_cast<SRV11 *>(*out)->p->GetDesc(&(*out)->desc);
                return hr;
            }
            HRESULT CreateRenderTargetView(Resource *r, const D3D11_RENDER_TARGET_VIEW_DESC *desc, RTV **out) override
            {
                HRESULT hr = view<RTV11>(r, desc, out, [&](ID3D11Resource *n, auto *dd, auto **p) { return d->CreateRenderTargetView(n, dd, p); });
                if (SUCCEEDED(hr))
                    (*out)->format = desc ? desc->Format : as_texture(r) ? as_texture(r)->desc.Format : DXGI_FORMAT_UNKNOWN;
                return hr;
            }
            HRESULT CreateDepthStencilView(Resource *r, const D3D11_DEPTH_STENCIL_VIEW_DESC *desc, DSV **out) override
            {
                HRESULT hr = view<DSV11>(r, desc, out, [&](ID3D11Resource *n, auto *dd, auto **p) { return d->CreateDepthStencilView(n, dd, p); });
                if (SUCCEEDED(hr))
                    (*out)->format = desc ? desc->Format : DXGI_FORMAT_UNKNOWN;
                return hr;
            }
            template <class W, class Base, class Fn> HRESULT make(Base **out, Fn create)
            {
                auto *o = new W;
                HRESULT hr = create(&o->p);
                if (FAILED(hr))
                    return delete o, *out = nullptr, hr;
                *out = o;
                return hr;
            }
            HRESULT CreateVertexShader(const void *code, SIZE_T size, void *, VertexShader **out) override
            {
                return make<VS11>(out, [&](auto **p) { return d->CreateVertexShader(code, size, nullptr, p); });
            }
            HRESULT CreatePixelShader(const void *code, SIZE_T size, void *, PixelShader **out) override
            {
                return make<PS11>(out, [&](auto **p) { return d->CreatePixelShader(code, size, nullptr, p); });
            }
            HRESULT CreateBlendState(const D3D11_BLEND_DESC *desc, BlendState **out) override
            {
                HRESULT hr = make<Blend11>(out, [&](auto **p) { return d->CreateBlendState(desc, p); });
                if (SUCCEEDED(hr))
                    (*out)->desc = *desc;
                return hr;
            }
            HRESULT CreateDepthStencilState(const D3D11_DEPTH_STENCIL_DESC *desc, DepthStencilState **out) override
            {
                HRESULT hr = make<Depth11>(out, [&](auto **p) { return d->CreateDepthStencilState(desc, p); });
                if (SUCCEEDED(hr))
                    (*out)->desc = *desc;
                return hr;
            }
            HRESULT CreateRasterizerState(const D3D11_RASTERIZER_DESC *desc, RasterizerState **out) override
            {
                HRESULT hr = make<Raster11>(out, [&](auto **p) { return d->CreateRasterizerState(desc, p); });
                if (SUCCEEDED(hr))
                    (*out)->desc = *desc;
                return hr;
            }
            HRESULT CreateSamplerState(const D3D11_SAMPLER_DESC *desc, SamplerState **out) override
            {
                HRESULT hr = make<Sampler11>(out, [&](auto **p) { return d->CreateSamplerState(desc, p); });
                if (SUCCEEDED(hr))
                    (*out)->desc = *desc;
                return hr;
            }
            HRESULT CreateQuery(const D3D11_QUERY_DESC *desc, Query **out) override
            {
                HRESULT hr = make<Query11>(out, [&](auto **p) { return d->CreateQuery(desc, p); });
                if (SUCCEEDED(hr))
                    (*out)->desc = *desc;
                return hr;
            }
            bool NoOverwriteOnBufferSRV() override { return no_overwrite_srv; }
            bool ConstantBufferOffsets() override { return cb_offsets; }
            const char *Name() override { return "DX11"; }
        };

        struct Context11 final : Context
        {
            ID3D11DeviceContext *c = nullptr;
            ID3D11DeviceContext1 *c1 = nullptr;
            RTV *bound_rtv[8] = {};
            DSV *bound_dsv = nullptr;

            HRESULT Map(Resource *r, UINT sub, D3D11_MAP type, UINT flags, D3D11_MAPPED_SUBRESOURCE *out) override
            {
                return c->Map(native(r), sub, type, flags, out);
            }
            void Unmap(Resource *r, UINT sub) override { c->Unmap(native(r), sub); }
            void UpdateSubresource(Resource *r, UINT sub, const D3D11_BOX *box, const void *data, UINT rp, UINT dp) override
            {
                c->UpdateSubresource(native(r), sub, box, data, rp, dp);
            }
            void CopyResource(Resource *dst, Resource *src) override { c->CopyResource(native(dst), native(src)); }
            void CopySubresourceRegion(Resource *dst, UINT ds, UINT x, UINT y, UINT z, Resource *src, UINT ss, const D3D11_BOX *box) override
            {
                c->CopySubresourceRegion(native(dst), ds, x, y, z, native(src), ss, box);
            }
            void VSSetShader(VertexShader *vs, void *, UINT) override { c->VSSetShader(nat<VS11>(vs), nullptr, 0); }
            void PSSetShader(PixelShader *ps, void *, UINT) override { c->PSSetShader(nat<PS11>(ps), nullptr, 0); }
            template <class W, class T, class N> static void unwrap(UINT n, T *const *in, N **out)
            {
                for (UINT i = 0; i < n; i++)
                    out[i] = in ? nat<W>(in[i]) : nullptr;
            }
            void VSSetShaderResources(UINT s, UINT n, SRV *const *v) override
            {
                ID3D11ShaderResourceView *x[16];
                unwrap<SRV11>(n, v, x);
                c->VSSetShaderResources(s, n, x);
            }
            void PSSetShaderResources(UINT s, UINT n, SRV *const *v) override
            {
                ID3D11ShaderResourceView *x[16];
                unwrap<SRV11>(n, v, x);
                c->PSSetShaderResources(s, n, x);
            }
            void PSSetSamplers(UINT s, UINT n, SamplerState *const *v) override
            {
                ID3D11SamplerState *x[16];
                unwrap<Sampler11>(n, v, x);
                c->PSSetSamplers(s, n, x);
            }
            void VSSetConstantBuffers(UINT s, UINT n, Buffer *const *v) override
            {
                ID3D11Buffer *x[16];
                unwrap<Buf11>(n, v, x);
                c->VSSetConstantBuffers(s, n, x);
            }
            void PSSetConstantBuffers(UINT s, UINT n, Buffer *const *v) override
            {
                ID3D11Buffer *x[16];
                unwrap<Buf11>(n, v, x);
                c->PSSetConstantBuffers(s, n, x);
            }
            void VSSetConstantBuffers1(UINT s, UINT n, Buffer *const *v, const UINT *first, const UINT *num) override
            {
                ID3D11Buffer *x[16];
                unwrap<Buf11>(n, v, x);
                c1->VSSetConstantBuffers1(s, n, x, first, num);
            }
            void PSSetConstantBuffers1(UINT s, UINT n, Buffer *const *v, const UINT *first, const UINT *num) override
            {
                ID3D11Buffer *x[16];
                unwrap<Buf11>(n, v, x);
                c1->PSSetConstantBuffers1(s, n, x, first, num);
            }
            void IASetIndexBuffer(Buffer *b, DXGI_FORMAT f, UINT o) override { c->IASetIndexBuffer(nat<Buf11>(b), f, o); }
            void IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY t) override { c->IASetPrimitiveTopology(t); }
            void RSSetState(RasterizerState *s) override { c->RSSetState(nat<Raster11>(s)); }
            void RSSetViewports(UINT n, const D3D11_VIEWPORT *vp) override { c->RSSetViewports(n, vp); }
            void RSGetViewports(UINT *n, D3D11_VIEWPORT *vp) override { c->RSGetViewports(n, vp); }
            void RSSetScissorRects(UINT n, const D3D11_RECT *r) override { c->RSSetScissorRects(n, r); }
            void OMSetRenderTargets(UINT n, RTV *const *rtvs, DSV *dsv) override
            {
                ID3D11RenderTargetView *x[8] = {};
                unwrap<RTV11>(n, rtvs, x);
                c->OMSetRenderTargets(n, x, nat<DSV11>(dsv));
                // Remember what is bound (with references) for OMGetRenderTargets.
                for (UINT i = 0; i < 8; i++)
                {
                    RTV *v = i < n && rtvs ? rtvs[i] : nullptr;
                    if (v)
                        v->AddRef();
                    if (bound_rtv[i])
                        bound_rtv[i]->Release();
                    bound_rtv[i] = v;
                }
                if (dsv)
                    dsv->AddRef();
                if (bound_dsv)
                    bound_dsv->Release();
                bound_dsv = dsv;
            }
            void OMGetRenderTargets(UINT n, RTV **rtvs, DSV **dsv) override
            {
                for (UINT i = 0; i < n; i++)
                    if ((rtvs[i] = bound_rtv[i]))
                        rtvs[i]->AddRef();
                if (dsv && (*dsv = bound_dsv))
                    bound_dsv->AddRef();
            }
            void OMSetBlendState(BlendState *s, const FLOAT f[4], UINT m) override { c->OMSetBlendState(nat<Blend11>(s), f, m); }
            void OMSetDepthStencilState(DepthStencilState *s, UINT ref) override { c->OMSetDepthStencilState(nat<Depth11>(s), ref); }
            void Draw(UINT n, UINT s) override { c->Draw(n, s); }
            void DrawIndexed(UINT n, UINT s, INT b) override { c->DrawIndexed(n, s, b); }
            void ClearRenderTargetView(RTV *v, const FLOAT col[4]) override { c->ClearRenderTargetView(nat<RTV11>(v), col); }
            void ClearDepthStencilView(DSV *v, UINT f, FLOAT d, UINT8 s) override { c->ClearDepthStencilView(nat<DSV11>(v), f, d, s); }
            void Begin(Query *q) override { c->Begin(nat<Query11>(q)); }
            void End(Query *q) override { c->End(nat<Query11>(q)); }
            HRESULT GetData(Query *q, void *data, UINT size, UINT flags) override { return c->GetData(nat<Query11>(q), data, size, flags); }
            void Flush() override { c->Flush(); }
        };

        // Blt model, one buffer: stretches to the window and screen capture tools see it.
        struct Swap11 final : Swap
        {
            IDXGISwapChain *s = nullptr;
            Device11 *dev = nullptr;
            RTV *rtv = nullptr;
            bool make_rtv()
            {
                ID3D11Texture2D *bb = nullptr;
                if (FAILED(s->GetBuffer(0, __uuidof(ID3D11Texture2D), (void **)&bb)))
                    return false;
                auto *t = new Tex11;
                t->p = bb;
                bb->GetDesc(&t->desc);
                HRESULT hr = dev->CreateRenderTargetView(t, nullptr, &rtv);
                t->Release();
                return SUCCEEDED(hr);
            }
            RTV *Backbuffer() override { return rtv; }
            void Resize(UINT w, UINT h) override
            {
                release(rtv);
                s->ResizeBuffers(0, w, h, DXGI_FORMAT_UNKNOWN, 0);
                make_rtv();
            }
            void Present(bool vsync) override { s->Present(vsync ? 1 : 0, 0); }
        };
    } // namespace

    bool create_d3d11(IDXGIAdapter1 *adapter, HWND hwnd, bool debug, Device **dev, Context **ctx, Swap **swap)
    {
        DXGI_SWAP_CHAIN_DESC sd{};
        sd.BufferCount = 1;
        sd.BufferDesc.Width = 1280, sd.BufferDesc.Height = 720;
        sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        sd.OutputWindow = hwnd;
        sd.SampleDesc.Count = 1;
        sd.Windowed = TRUE;
        sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;   // blt model: stretches to the window, capturable
        D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;
        auto *d = new Device11;
        auto *c = new Context11;
        auto *s = new Swap11;
        HRESULT hr = D3D11CreateDeviceAndSwapChain(adapter, adapter ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                                   debug ? D3D11_CREATE_DEVICE_DEBUG : 0, &fl, 1, D3D11_SDK_VERSION, &sd, &s->s,
                                                   &d->d, nullptr, &c->c);
        if (FAILED(hr))
        {
            LOG("rsx", "D3D11 device creation failed (0x%08lx)", hr);
            delete d, delete c, delete s;
            return false;
        }
        D3D11_FEATURE_DATA_D3D11_OPTIONS opts{};
        d->no_overwrite_srv = SUCCEEDED(d->d->CheckFeatureSupport(D3D11_FEATURE_D3D11_OPTIONS, &opts, sizeof opts)) &&
                              opts.MapNoOverwriteOnDynamicBufferSRV;
        // The constant ring (d3d.cpp cb_push) binds at offsets and maps NO_OVERWRITE.
        d->cb_offsets = opts.ConstantBufferOffsetting && opts.MapNoOverwriteOnDynamicConstantBuffer &&
                        SUCCEEDED(c->c->QueryInterface(__uuidof(ID3D11DeviceContext1), (void **)&c->c1));
        s->dev = d;
        if (!s->make_rtv())
            return false;
        *dev = d, *ctx = c, *swap = s;
        return true;
    }
} // namespace gpu
