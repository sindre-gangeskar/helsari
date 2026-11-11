// gpu.h on Direct3D 12 (Options > Graphics > Rendering API: DX12).
//
// The renderer (d3d.cpp) is written against a Direct3D 11-like immediate context; this file gives it
// one on Direct3D 12, made for the way that renderer works:
//
//  * One command list records everything; Flush() submits it and signals the fence. Every submission
//    has a fence value, and whatever the GPU may still read waits for its value before it is reused
//    or freed (deferred releases, upload memory, descriptors, buffer versions).
//  * Reading back (Map READ of a staging texture) waits only for the submission that copied into it -
//    never for the whole queue. With DO_NOT_WAIT it reports "still drawing" instead, as D3D11 does.
//  * Pipeline states are built from the bound shaders, the D3D11 state descriptions, the target
//    formats and the topology type, and cached. One root signature serves every draw: vertex constants
//    b0-b2 and pixel constants b0 as root CBVs, pixel textures t0-t15 and the vertex ring t0 as
//    descriptor tables, pixel samplers s0-s15 as a cached sampler table.
//  * Dynamic buffers live in upload memory: WRITE_DISCARD moves a buffer to a version the GPU is done
//    with (or a new one), NO_OVERWRITE keeps the current one.
//  * Textures start in COMMON and move between states as they are used (explicit barriers).
#include "gpu.h"
#include "runtime.h"

#include <d3d12.h>
#ifdef HELSARI_FFX
#include "api/include/ffx_api.h"
#include "api/include/dx12/ffx_api_dx12.h"
#include "upscalers/include/ffx_upscale.h"
#endif

#include <algorithm>
#include <array>
#include <cstring>
#include <deque>
#include <unordered_map>
#include <vector>

namespace gpu
{
    namespace
    {
        template <class T> void release(T *&p)
        {
            if (p)
                p->Release(), p = nullptr;
        }
        uint64_t mix(uint64_t h, uint64_t v) { return (h ^ v) * 0x100000001b3ull + (h >> 29); }
        uint64_t hash_bytes(const void *p, size_t n, uint64_t h = 1469598103934665603ull)
        {
            const uint8_t *b = (const uint8_t *)p;
            for (size_t i = 0; i < n; i++)
                h = (h ^ b[i]) * 1099511628211ull;
            return h;
        }

        // Costs for the fps log (Device12::Stats).
        uint64_t st_create_ns, st_creates, st_pso_ns, st_psos, st_up_bytes, st_up_wait_ns, st_map_wait_ns, st_submits;
        std::unordered_map<uint64_t, uint64_t> st_what;   // created: (kind, w, h, format) -> count
        void count_create(uint64_t kind, uint64_t w, uint64_t h, uint64_t fmt) { st_what[kind << 60 | w << 40 | h << 20 | fmt]++; }
        uint64_t now_ns()
        {
            LARGE_INTEGER c, f;
            QueryPerformanceCounter(&c), QueryPerformanceFrequency(&f);
            return (uint64_t)((double)c.QuadPart * 1e9 / (double)f.QuadPart);
        }

        constexpr D3D12_RESOURCE_STATES SRV_STATE = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

        // Small textures are placed in shared heaps instead of each getting its own allocation: the
        // game's SPUs leave a hundred or more 8x8 data textures per frame at new addresses, and a
        // committed resource costs ~0.4 ms to create and 64 KB of memory. Blocks come in size classes;
        // a freed block is reused once the GPU is past the submission that last used it.
        struct Block
        {
            ID3D12Heap *heap = nullptr;
            uint64_t offset = 0, size = 0;
        };
        constexpr uint64_t PLACED_HEAP = 16ull << 20, PLACED_MAX = 1ull << 20;
        struct SizeClass
        {
            std::vector<Block> free;
            ID3D12Heap *heap = nullptr;
            uint64_t top = 0;
        };
        std::unordered_map<uint64_t, SizeClass> g_classes;   // by block size
        std::deque<std::pair<uint64_t, Block>> g_blocks_freed;  // (fence, block)

        // ------------------------------------------------------------------ device-wide state
        struct Backend
        {
            ID3D12Device *dev = nullptr;
            ID3D12CommandQueue *queue = nullptr;
            ID3D12Fence *fence = nullptr;
            HANDLE fence_event = nullptr;
            uint64_t next = 1;   // the fence value the open command list will signal
            ID3D12GraphicsCommandList *list = nullptr;
            ID3D12CommandAllocator *alloc = nullptr;
            std::deque<std::pair<uint64_t, ID3D12CommandAllocator *>> spare_allocs;
            bool recorded = false;   // the open list has commands
            uint64_t gen = 0;        // submissions so far: a context sets its state again on a new list
            struct Query12 *active_occ = nullptr;   // an occlusion query open across a submission
            std::deque<std::pair<uint64_t, IUnknown *>> deferred;   // released when the fence passes

            ID3D12RootSignature *root = nullptr;
            UINT srv_size = 0, rtv_size = 0, dsv_size = 0, smp_size = 0;

            bool done(uint64_t v) { return fence->GetCompletedValue() >= v; }
            void wait(uint64_t v)
            {
                if (done(v))
                    return;
                fence->SetEventOnCompletion(v, fence_event);
                WaitForSingleObject(fence_event, INFINITE);
            }
            void defer(IUnknown *p)
            {
                if (p)
                    deferred.push_back({next, p});
            }
            void collect()
            {
                const uint64_t c = fence->GetCompletedValue();
                while (!deferred.empty() && deferred.front().first <= c)
                    deferred.front().second->Release(), deferred.pop_front();
                while (!g_blocks_freed.empty() && g_blocks_freed.front().first <= c)
                {
                    const Block &b = g_blocks_freed.front().second;
                    g_classes[b.size].free.push_back(b);
                    g_blocks_freed.pop_front();
                }
            }
            void submit();
            void idle()
            {
                submit();
                wait(next - 1);
                collect();
            }
        } g;

        // CPU-only descriptor heap with a free list (views keep their descriptors while they live).
        struct CpuHeap
        {
            ID3D12DescriptorHeap *heap = nullptr;
            UINT size = 0, inc = 0, top = 0;
            std::vector<UINT> free;
            void init(D3D12_DESCRIPTOR_HEAP_TYPE type, UINT n)
            {
                D3D12_DESCRIPTOR_HEAP_DESC d{type, n, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
                g.dev->CreateDescriptorHeap(&d, __uuidof(ID3D12DescriptorHeap), (void **)&heap);
                size = n, inc = g.dev->GetDescriptorHandleIncrementSize(type);
            }
            UINT alloc()
            {
                if (!free.empty())
                {
                    UINT i = free.back();
                    free.pop_back();
                    return i;
                }
                if (top == size)
                    LOG("rsx", "DX12: CPU descriptor heap full (%u)", size);
                return top < size ? top++ : 0;
            }
            void release(UINT i) { free.push_back(i); }
            D3D12_CPU_DESCRIPTOR_HANDLE cpu(UINT i) const { return {heap->GetCPUDescriptorHandleForHeapStart().ptr + (SIZE_T)i * inc}; }
        };
        CpuHeap g_srv_cpu, g_rtv_cpu, g_dsv_cpu;

        // A ring over a resource or heap: allocations made before a submission are reused after its
        // fence passes.
        struct Ring
        {
            uint64_t size = 0, head = 0, tail = 0, used = 0;
            std::deque<std::pair<uint64_t, uint64_t>> marks;   // (fence, head at that submission)
            void mark(uint64_t fence) { marks.push_back({fence, head}); }
            // Offset of n units aligned to `align`, waiting for the GPU when the ring is full.
            uint64_t alloc(uint64_t n, uint64_t align)
            {
                for (;;)
                {
                    uint64_t at = (head + align - 1) / align * align;
                    const bool wraps = at + n > size;
                    if (wraps)
                        at = 0;
                    const uint64_t need = (wraps ? size - head : at - head) + n;
                    if (used + need <= size)
                    {
                        used += need;
                        head = at + n;
                        if (head == size)
                            head = 0;
                        return at;
                    }
                    if (marks.empty())
                        g.recorded = true, g.submit();   // everything in use belongs to the open list
                    // Free the oldest submission's part (waiting for it).
                    auto [f, h] = marks.front();
                    marks.pop_front();
                    const uint64_t t0 = now_ns();
                    g.wait(f);
                    st_up_wait_ns += now_ns() - t0;
                    used -= (h >= tail ? h - tail : size - tail + h);
                    tail = h;
                }
            }
            void retire()   // free whatever completed submissions held, without waiting
            {
                while (!marks.empty() && g.done(marks.front().first))
                {
                    auto [f, h] = marks.front();
                    marks.pop_front();
                    used -= (h >= tail ? h - tail : size - tail + h);
                    tail = h;
                }
            }
        };

        // Upload memory for texture updates.
        struct Upload
        {
            ID3D12Resource *res = nullptr;
            uint8_t *ptr = nullptr;
            Ring ring;
        } g_up;

        // Shader-visible descriptors: a ring of CBV/SRV/UAV descriptors, and a table cache of samplers.
        struct GpuHeap
        {
            ID3D12DescriptorHeap *heap = nullptr;
            UINT inc = 0;
            Ring ring;
            D3D12_CPU_DESCRIPTOR_HANDLE cpu(uint64_t i) const { return {heap->GetCPUDescriptorHandleForHeapStart().ptr + (SIZE_T)i * inc}; }
            D3D12_GPU_DESCRIPTOR_HANDLE gpu(uint64_t i) const { return {heap->GetGPUDescriptorHandleForHeapStart().ptr + i * inc}; }
        } g_view_heap;
        constexpr UINT SAMPLER_HEAP = 2048;
        ID3D12DescriptorHeap *g_smp_heap;
        UINT g_smp_top;
        // Cache keys hold the exact values and are compared in full: a lookup hit is never a different
        // combination (a 64-bit mix of nearby pointers can collide, and a collision draws with the wrong
        // pipeline or samplers).
        template <size_t N> struct Key
        {
            std::array<uint64_t, N> v{};
            bool operator==(const Key &o) const { return v == o.v; }
        };
        struct KeyHash
        {
            template <size_t N> size_t operator()(const Key<N> &k) const { return (size_t)hash_bytes(k.v.data(), N * 8); }
        };
        std::unordered_map<Key<16>, UINT, KeyHash> g_smp_tables;   // 16 samplers -> first descriptor

        // ------------------------------------------------------------------ objects
        struct Tex12 final : Texture2D
        {
            ID3D12Resource *res = nullptr;
            D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COMMON;
            // Staging (CPU read): a read-back buffer holding each subresource at its footprint.
            bool staging = false;
            std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> fp;
            std::vector<UINT> rows;
            uint8_t *mapped = nullptr;
            uint64_t written = 0;   // staging: the submission that last copied into it
            bool swapchain = false;
            Block block;   // placed in a shared heap (block.heap set)
            ~Tex12() override
            {
                if (swapchain)
                    release(res);   // only released while the GPU is idle (Swap12::Resize)
                else
                    g.defer(res);
                if (block.heap)
                    g_blocks_freed.push_back({g.next, block});
            }
        };
        struct Buf12 final : Buffer
        {
            struct Version
            {
                ID3D12Resource *res = nullptr;
                uint8_t *ptr = nullptr;
                uint64_t fence = 0;   // retired: the last submission that may read it
            };
            Version cur;
            std::vector<Version> spare;
            ~Buf12() override
            {
                g.defer(cur.res);
                for (auto &v : spare)
                    g.defer(v.res);
            }
            D3D12_GPU_VIRTUAL_ADDRESS va() const { return cur.res->GetGPUVirtualAddress(); }
            bool new_version()
            {
                for (size_t i = 0; i < spare.size(); i++)
                    if (g.done(spare[i].fence))
                    {
                        Version v = spare[i];
                        spare.erase(spare.begin() + i);
                        if (cur.res)
                            cur.fence = g.next, spare.push_back(cur);
                        cur = v;
                        return true;
                    }
                D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_UPLOAD};
                D3D12_RESOURCE_DESC rd{D3D12_RESOURCE_DIMENSION_BUFFER, 0, (desc.ByteWidth + 255) & ~255u, 1, 1, 1, DXGI_FORMAT_UNKNOWN, {1, 0},
                                       D3D12_TEXTURE_LAYOUT_ROW_MAJOR, D3D12_RESOURCE_FLAG_NONE};
                Version v;
                const uint64_t t0 = now_ns();
                st_creates++;
                count_create(4, desc.ByteWidth, desc.BindFlags, 0);
                struct Timed { uint64_t t0; ~Timed() { st_create_ns += now_ns() - t0; } } timed{t0};
                if (FAILED(g.dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                          __uuidof(ID3D12Resource), (void **)&v.res)))
                    return false;
                D3D12_RANGE none{0, 0};
                v.res->Map(0, &none, (void **)&v.ptr);
                if (cur.res)
                    cur.fence = g.next, spare.push_back(cur);
                cur = v;
                return true;
            }
        };
        struct SRV12 final : SRV
        {
            UINT slot = 0;
            D3D12_SHADER_RESOURCE_VIEW_DESC d12{};
            ~SRV12() override { g_srv_cpu.release(slot); }
        };
        struct RTV12 final : RTV
        {
            UINT slot = 0;
            ~RTV12() override { g_rtv_cpu.release(slot); }
        };
        struct DSV12 final : DSV
        {
            UINT slot = 0;
            ~DSV12() override { g_dsv_cpu.release(slot); }
        };
        struct VS12 final : VertexShader { std::vector<uint8_t> code; };
        struct PS12 final : PixelShader { std::vector<uint8_t> code; };
        struct Sampler12 final : SamplerState { UINT id = 0; };
        struct QueryHeap;
        struct Query12 final : Query
        {
            // Occlusion: one slot per command list it spans (a query cannot cross a submission); the
            // result is their sum. Timestamps: one slot.
            std::vector<UINT> slots;
            uint64_t fence = 0;   // the submission that resolved its last slot (0: not ended)
            QueryHeap *heap = nullptr;
            ~Query12() override;
        };

        // Query heaps: one result slot per query, resolved into read-back memory.
        struct QueryHeap
        {
            ID3D12QueryHeap *heap = nullptr;
            ID3D12Resource *results = nullptr;
            uint64_t *mapped = nullptr;
            UINT top = 0;
            std::vector<UINT> free;
            void init(D3D12_QUERY_HEAP_TYPE type, UINT n)
            {
                D3D12_QUERY_HEAP_DESC d{type, n, 0};
                g.dev->CreateQueryHeap(&d, __uuidof(ID3D12QueryHeap), (void **)&heap);
                D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_READBACK};
                D3D12_RESOURCE_DESC rd{D3D12_RESOURCE_DIMENSION_BUFFER, 0, (UINT64)n * 8, 1, 1, 1, DXGI_FORMAT_UNKNOWN, {1, 0},
                                       D3D12_TEXTURE_LAYOUT_ROW_MAJOR, D3D12_RESOURCE_FLAG_NONE};
                g.dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, __uuidof(ID3D12Resource),
                                               (void **)&results);
                results->Map(0, nullptr, (void **)&mapped);
            }
            static constexpr UINT QUERY_SLOTS = 65536;
            UINT take()
            {
                if (!free.empty())
                {
                    UINT i = free.back();
                    free.pop_back();
                    return i;
                }
                return top < QUERY_SLOTS ? top++ : 0;
            }
        } g_occ, g_time;
        constexpr UINT QUERY_SLOTS = QueryHeap::QUERY_SLOTS;
        Query12::~Query12()
        {
            if (g.active_occ == this)
                g.active_occ = nullptr;
            if (heap)
                for (UINT i : slots)
                    heap->free.push_back(i);
        }

        void Backend::submit()
        {
            if (!recorded)
                return;
            // An open occlusion query ends here and continues in a new slot on the next list.
            Query12 *q = active_occ;
            if (q)
            {
                list->EndQuery(g_occ.heap, D3D12_QUERY_TYPE_OCCLUSION, q->slots.back());
                list->ResolveQueryData(g_occ.heap, D3D12_QUERY_TYPE_OCCLUSION, q->slots.back(), 1, g_occ.results, (UINT64)q->slots.back() * 8);
            }
            list->Close();
            st_submits++;
            ID3D12CommandList *l[] = {list};
            queue->ExecuteCommandLists(1, l);
            queue->Signal(fence, next);
            spare_allocs.push_back({next, alloc});
            g_up.ring.mark(next);
            g_view_heap.ring.mark(next);
            next++;
            alloc = nullptr;
            if (!spare_allocs.empty() && done(spare_allocs.front().first))
                alloc = spare_allocs.front().second, spare_allocs.pop_front(), alloc->Reset();
            else
                dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, __uuidof(ID3D12CommandAllocator), (void **)&alloc);
            list->Reset(alloc, nullptr);
            recorded = false;
            gen++;
            if (q)
            {
                q->slots.push_back(g_occ.take());
                list->BeginQuery(g_occ.heap, D3D12_QUERY_TYPE_OCCLUSION, q->slots.back());
                recorded = true;
            }
            collect();
            g_up.ring.retire();
            g_view_heap.ring.retire();
        }


        ID3D12Resource *g_zero_cb;   // bound where no constant buffer is (256 zero bytes)
        D3D12_CPU_DESCRIPTOR_HANDLE g_null_srv;

        // ------------------------------------------------------------------ helpers
        void barrier(std::vector<D3D12_RESOURCE_BARRIER> &out, Tex12 *t, D3D12_RESOURCE_STATES to)
        {
            if (!t || t->staging || t->state == to)
                return;
            // Read states combine; a resource already readable stays put for another read.
            if (to == SRV_STATE && (t->state & SRV_STATE) == SRV_STATE)
                return;
            D3D12_RESOURCE_BARRIER b{};
            b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            b.Transition.pResource = t->res;
            b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            b.Transition.StateBefore = t->state;
            b.Transition.StateAfter = to;
            out.push_back(b);
            t->state = to;
        }
        void flush_barriers(std::vector<D3D12_RESOURCE_BARRIER> &b)
        {
            if (!b.empty())
                g.list->ResourceBarrier((UINT)b.size(), b.data()), g.recorded = true, b.clear();
        }
        Tex12 *tex_of(Resource *r) { return r && r->kind == Resource::TEXTURE2D ? static_cast<Tex12 *>(r) : nullptr; }

        // Copies `rows` rows of `row_bytes` into upload memory with D3D12's row alignment, then into the subresource.
        void upload_subresource(Tex12 *t, UINT sub, const D3D11_BOX *box, const void *data, UINT row_pitch)
        {
            D3D12_RESOURCE_DESC rd = t->res->GetDesc();
            D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp;
            UINT rows;
            UINT64 row_bytes, total;
            g.dev->GetCopyableFootprints(&rd, sub, 1, 0, &fp, &rows, &row_bytes, &total);
            UINT w = fp.Footprint.Width, h = fp.Footprint.Height;
            if (box)
            {
                // A box changes how many rows and bytes per row: scale from the full footprint.
                const UINT bw = box->right - box->left, bh = box->bottom - box->top;
                const bool bc = rd.Format >= DXGI_FORMAT_BC1_TYPELESS && rd.Format <= DXGI_FORMAT_BC5_SNORM;
                row_bytes = row_bytes * bw / w;
                rows = bc ? (bh + 3) / 4 : bh;
                w = bw, h = bh;
            }
            const UINT pitch = (UINT)((row_bytes + 255) & ~255ull);
            const uint64_t off = g_up.ring.alloc((uint64_t)pitch * rows, 512);
            st_up_bytes += (uint64_t)pitch * rows;
            for (UINT y = 0; y < rows; y++)
                memcpy(g_up.ptr + off + (uint64_t)y * pitch, (const uint8_t *)data + (size_t)y * row_pitch, (size_t)row_bytes);
            D3D12_TEXTURE_COPY_LOCATION src{g_up.res, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
            src.PlacedFootprint.Offset = off;
            src.PlacedFootprint.Footprint = {fp.Footprint.Format, w, h, 1, pitch};
            D3D12_TEXTURE_COPY_LOCATION dst{t->res, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
            dst.SubresourceIndex = sub;
            std::vector<D3D12_RESOURCE_BARRIER> b;
            barrier(b, t, D3D12_RESOURCE_STATE_COPY_DEST);
            flush_barriers(b);
            g.list->CopyTextureRegion(&dst, box ? box->left : 0, box ? box->top : 0, 0, &src, nullptr);
            g.recorded = true;
        }

#ifdef HELSARI_FFX
        // AMD FidelityFX Super Resolution, through AMD's signed DLLs next to the executable: the loader
        // finds the upscaler DLL, which offers FSR 3.1 and, on cards that support it, FSR 4.
        void fsr_message(uint32_t type, const wchar_t *msg)
        {
            char b[512];
            WideCharToMultiByte(CP_UTF8, 0, msg, -1, b, sizeof b, nullptr, nullptr);
            LOG("rsx", "FSR %s: %s", type == FFX_API_MESSAGE_TYPE_ERROR ? "error" : "warning", b);
        }
        struct Fsr
        {
            bool tried = false;
            HMODULE dll = nullptr;
            PfnFfxCreateContext create = nullptr;
            PfnFfxDestroyContext destroy = nullptr;
            PfnFfxQuery query = nullptr;
            PfnFfxDispatch dispatch = nullptr;
            ffxContext ctx = nullptr;
            UINT rw = 0, rh = 0, ow = 0, oh = 0;
            bool inverted = false;
            std::string name;   // the version in use, or the newest available before a context exists

            bool load()
            {
                if (tried)
                    return create != nullptr;
                tried = true;
                wchar_t path[MAX_PATH];
                GetModuleFileNameW(nullptr, path, MAX_PATH);
                std::wstring p = path;
                p = p.substr(0, p.find_last_of(L"\\/") + 1) + L"amd_fidelityfx_loader_dx12.dll";
                dll = LoadLibraryW(p.c_str());
                if (!dll)
                {
                    LOG("rsx", "FSR: amd_fidelityfx_loader_dx12.dll not found next to the executable");
                    return false;
                }
                create = (PfnFfxCreateContext)GetProcAddress(dll, "ffxCreateContext");
                destroy = (PfnFfxDestroyContext)GetProcAddress(dll, "ffxDestroyContext");
                query = (PfnFfxQuery)GetProcAddress(dll, "ffxQuery");
                dispatch = (PfnFfxDispatch)GetProcAddress(dll, "ffxDispatch");
                if (!create || !destroy || !query || !dispatch)
                {
                    create = nullptr;
                    LOG("rsx", "FSR: the loader DLL lacks the FFX API");
                    return false;
                }
                // The versions this device can use, newest first.
                uint64_t count = 0;
                ffxQueryDescGetVersions q{};
                q.header.type = FFX_API_QUERY_DESC_TYPE_GET_VERSIONS;
                q.createDescType = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
                q.device = g.dev;
                q.outputCount = &count;
                query(nullptr, &q.header);
                std::vector<uint64_t> ids(count);
                std::vector<const char *> names(count);
                q.versionIds = ids.data(), q.versionNames = names.data();
                if (count)
                    query(nullptr, &q.header);
                std::string all;
                for (uint64_t i = 0; i < count; i++)
                    all += std::string(i ? ", " : "") + (names[i] ? names[i] : "?");
                if (count && names[0])
                    name = std::string("FSR ") + names[0];
                LOG("rsx", "FSR: available versions: %s", all.empty() ? "none" : all.c_str());
                return true;
            }
            bool ensure(UINT r_w, UINT r_h, UINT o_w, UINT o_h, bool inv)
            {
                if (ctx && r_w <= rw && r_h <= rh && o_w == ow && o_h == oh && inv == inverted)
                    return true;
                if (!load())
                    return false;
                if (ctx)
                    g.idle(), destroy(&ctx, nullptr), ctx = nullptr;
                ffxCreateBackendDX12Desc be{};
                be.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12;
                be.device = g.dev;
                ffxCreateContextDescUpscale d{};
                d.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
                d.header.pNext = &be.header;
                d.flags = FFX_UPSCALE_ENABLE_NON_LINEAR_COLORSPACE | (inv ? FFX_UPSCALE_ENABLE_DEPTH_INVERTED : 0);
                d.maxRenderSize = {r_w, r_h};
                d.maxUpscaleSize = {o_w, o_h};
                d.fpMessage = fsr_message;
                const ffxReturnCode_t rc = create(&ctx, &d.header, nullptr);
                if (rc != FFX_API_RETURN_OK)
                {
                    LOG("rsx", "FSR: context creation failed (%u) for %ux%u -> %ux%u", rc, r_w, r_h, o_w, o_h);
                    ctx = nullptr;
                    return false;
                }
                rw = r_w, rh = r_h, ow = o_w, oh = o_h, inverted = inv;
                ffxQueryGetProviderVersion pv{};
                pv.header.type = FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION;
                query(&ctx, &pv.header);
                if (pv.versionName)
                    name = std::string("FSR ") + pv.versionName;
                LOG("rsx", "FSR: %s, %ux%u -> %ux%u", name.c_str(), r_w, r_h, o_w, o_h);
                return true;
            }
        } g_fsr;
#endif

        // ------------------------------------------------------------------ device
        struct Device12 final : Device
        {
            HRESULT CreateTexture2D(const D3D11_TEXTURE2D_DESC *d, const D3D11_SUBRESOURCE_DATA *init, Texture2D **out) override
            {
                auto *t = new Tex12;
                t->desc = *d;
                D3D12_RESOURCE_DESC rd{D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, d->Width, d->Height, (UINT16)d->ArraySize,
                                       (UINT16)d->MipLevels, d->Format, {1, 0}, D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_RESOURCE_FLAG_NONE};
                HRESULT hr;
                if (d->Usage == D3D11_USAGE_STAGING)
                {
                    // CPU-readable copy: a read-back buffer laid out like the texture's footprints.
                    t->staging = true;
                    const UINT subs = d->MipLevels * d->ArraySize;
                    t->fp.resize(subs), t->rows.resize(subs);
                    std::vector<UINT64> rb(subs);
                    UINT64 total = 0;
                    g.dev->GetCopyableFootprints(&rd, 0, subs, 0, t->fp.data(), t->rows.data(), rb.data(), &total);
                    D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_READBACK};
                    D3D12_RESOURCE_DESC bd{D3D12_RESOURCE_DIMENSION_BUFFER, 0, total, 1, 1, 1, DXGI_FORMAT_UNKNOWN, {1, 0},
                                           D3D12_TEXTURE_LAYOUT_ROW_MAJOR, D3D12_RESOURCE_FLAG_NONE};
                    hr = g.dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                        __uuidof(ID3D12Resource), (void **)&t->res);
                    if (SUCCEEDED(hr))
                        t->res->Map(0, nullptr, (void **)&t->mapped);
                }
                else
                {
                    if (d->BindFlags & D3D11_BIND_RENDER_TARGET)
                        rd.Flags |= D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
                    if (d->BindFlags & D3D11_BIND_DEPTH_STENCIL)
                        rd.Flags |= D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
                    if (d->BindFlags & D3D11_BIND_UNORDERED_ACCESS)
                        rd.Flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
                    D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_DEFAULT};
                    const uint64_t t0 = now_ns();
                    hr = E_FAIL;
                    if (!(d->BindFlags & (D3D11_BIND_RENDER_TARGET | D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_UNORDERED_ACCESS)))
                    {
                        // Small enough: a block of a shared heap (4 KB alignment where the texture allows it).
                        rd.Alignment = D3D12_SMALL_RESOURCE_PLACEMENT_ALIGNMENT;
                        D3D12_RESOURCE_ALLOCATION_INFO info = g.dev->GetResourceAllocationInfo(0, 1, &rd);
                        if (info.Alignment != D3D12_SMALL_RESOURCE_PLACEMENT_ALIGNMENT)
                            rd.Alignment = 0, info = g.dev->GetResourceAllocationInfo(0, 1, &rd);
                        const uint64_t size = (info.SizeInBytes + info.Alignment - 1) / info.Alignment * info.Alignment;
                        if (size <= PLACED_MAX)
                        {
                            SizeClass &c = g_classes[size];
                            Block b;
                            if (!c.free.empty())
                                b = c.free.back(), c.free.pop_back();
                            else
                            {
                                if (!c.heap || c.top + size > PLACED_HEAP)
                                {
                                    D3D12_HEAP_DESC hd{PLACED_HEAP, {D3D12_HEAP_TYPE_DEFAULT}, D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT,
                                                       D3D12_HEAP_FLAG_ALLOW_ONLY_NON_RT_DS_TEXTURES};
                                    c.heap = nullptr, c.top = 0;
                                    g.dev->CreateHeap(&hd, __uuidof(ID3D12Heap), (void **)&c.heap);   // kept for the process
                                }
                                b = {c.heap, c.top, size};
                                c.top += size;
                            }
                            if (b.heap)
                            {
                                hr = g.dev->CreatePlacedResource(b.heap, b.offset, &rd, D3D12_RESOURCE_STATE_COMMON, nullptr,
                                                                 __uuidof(ID3D12Resource), (void **)&t->res);
                                if (SUCCEEDED(hr))
                                    t->block = b;
                                else
                                    c.free.push_back(b);
                            }
                        }
                        rd.Alignment = 0;
                    }
                    if (FAILED(hr))
                    {
                        hr = g.dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COMMON, nullptr,
                                                            __uuidof(ID3D12Resource), (void **)&t->res);
                        st_creates++;
                    }
                    st_create_ns += now_ns() - t0;
                    count_create(d->MiscFlags & D3D11_RESOURCE_MISC_TEXTURECUBE ? 2 : d->BindFlags & D3D11_BIND_RENDER_TARGET ? 3 : 1, d->Width,
                                 d->Height, d->Format);
                    if (SUCCEEDED(hr) && init)
                        for (UINT k = 0; k < d->MipLevels * d->ArraySize; k++)
                            upload_subresource(t, k, nullptr, init[k].pSysMem, init[k].SysMemPitch);
                }
                if (FAILED(hr))
                {
                    LOG("rsx", "DX12: texture %ux%u format %u failed (0x%08lx)", d->Width, d->Height, d->Format, hr);
                    t->Release();
                    *out = nullptr;
                    return hr;
                }
                *out = t;
                return S_OK;
            }
            HRESULT CreateBuffer(const D3D11_BUFFER_DESC *d, const D3D11_SUBRESOURCE_DATA *init, Buffer **out) override
            {
                auto *b = new Buf12;
                b->desc = *d;
                if (!b->new_version())
                {
                    b->Release();
                    *out = nullptr;
                    return E_OUTOFMEMORY;
                }
                memset(b->cur.ptr, 0, d->ByteWidth);
                if (init)
                    memcpy(b->cur.ptr, init->pSysMem, d->ByteWidth);
                *out = b;
                return S_OK;
            }
            HRESULT CreateShaderResourceView(Resource *r, const D3D11_SHADER_RESOURCE_VIEW_DESC *d, SRV **out) override
            {
                auto *v = new SRV12;
                D3D12_SHADER_RESOURCE_VIEW_DESC &s = v->d12;
                s.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                if (Tex12 *t = tex_of(r))
                {
                    const D3D11_TEXTURE2D_DESC &td = t->desc;
                    s.Format = d ? d->Format : td.Format;
                    const bool cube = d ? d->ViewDimension == D3D11_SRV_DIMENSION_TEXTURECUBE : (td.MiscFlags & D3D11_RESOURCE_MISC_TEXTURECUBE) != 0;
                    const UINT mips = d ? (cube ? d->TextureCube.MipLevels : d->Texture2D.MipLevels) : td.MipLevels;
                    if (cube)
                        s.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE, s.TextureCube.MipLevels = mips;
                    else if (td.ArraySize > 1)
                        s.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY, s.Texture2DArray.MipLevels = mips,
                        s.Texture2DArray.ArraySize = td.ArraySize;
                    else
                        s.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D, s.Texture2D.MipLevels = mips;
                    v->slot = g_srv_cpu.alloc();
                    g.dev->CreateShaderResourceView(t->res, &s, g_srv_cpu.cpu(v->slot));
                }
                else
                {
                    // Buffer views are written at each draw, for the buffer's current version.
                    auto *b = static_cast<Buf12 *>(r);
                    s.Format = DXGI_FORMAT_UNKNOWN;
                    s.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
                    s.Buffer.StructureByteStride = b->desc.StructureByteStride;
                    s.Buffer.NumElements = d ? d->Buffer.NumElements : b->desc.ByteWidth / std::max(1u, b->desc.StructureByteStride);
                    v->slot = g_srv_cpu.alloc();
                }
                if (d)
                    v->desc = *d;
                r->AddRef();
                v->resource = r;
                *out = v;
                return S_OK;
            }
            HRESULT CreateRenderTargetView(Resource *r, const D3D11_RENDER_TARGET_VIEW_DESC *d, RTV **out) override
            {
                Tex12 *t = tex_of(r);
                auto *v = new RTV12;
                v->slot = g_rtv_cpu.alloc();
                v->format = d ? d->Format : t->desc.Format;
                D3D12_RENDER_TARGET_VIEW_DESC rd{v->format, D3D12_RTV_DIMENSION_TEXTURE2D};
                g.dev->CreateRenderTargetView(t->res, &rd, g_rtv_cpu.cpu(v->slot));
                r->AddRef();
                v->resource = r;
                *out = v;
                return S_OK;
            }
            HRESULT CreateDepthStencilView(Resource *r, const D3D11_DEPTH_STENCIL_VIEW_DESC *d, DSV **out) override
            {
                Tex12 *t = tex_of(r);
                auto *v = new DSV12;
                v->slot = g_dsv_cpu.alloc();
                v->format = d ? d->Format : t->desc.Format;
                D3D12_DEPTH_STENCIL_VIEW_DESC dd{v->format, D3D12_DSV_DIMENSION_TEXTURE2D, D3D12_DSV_FLAG_NONE};
                g.dev->CreateDepthStencilView(t->res, &dd, g_dsv_cpu.cpu(v->slot));
                r->AddRef();
                v->resource = r;
                *out = v;
                return S_OK;
            }
            HRESULT CreateVertexShader(const void *code, SIZE_T size, void *, VertexShader **out) override
            {
                auto *s = new VS12;
                s->code.assign((const uint8_t *)code, (const uint8_t *)code + size);
                *out = s;
                return S_OK;
            }
            HRESULT CreatePixelShader(const void *code, SIZE_T size, void *, PixelShader **out) override
            {
                auto *s = new PS12;
                s->code.assign((const uint8_t *)code, (const uint8_t *)code + size);
                *out = s;
                return S_OK;
            }
            template <class T, class D> HRESULT state(const D *d, T **out)
            {
                auto *s = new T;
                s->desc = *d;
                *out = s;
                return S_OK;
            }
            HRESULT CreateBlendState(const D3D11_BLEND_DESC *d, BlendState **out) override { return state<BlendState>(d, out); }
            HRESULT CreateDepthStencilState(const D3D11_DEPTH_STENCIL_DESC *d, DepthStencilState **out) override
            {
                return state<DepthStencilState>(d, out);
            }
            HRESULT CreateRasterizerState(const D3D11_RASTERIZER_DESC *d, RasterizerState **out) override
            {
                return state<RasterizerState>(d, out);
            }
            HRESULT CreateSamplerState(const D3D11_SAMPLER_DESC *d, SamplerState **out) override
            {
                static UINT ids;
                auto *s = new Sampler12;
                s->desc = *d;
                s->id = ++ids;
                *out = s;
                return S_OK;
            }
            HRESULT CreateQuery(const D3D11_QUERY_DESC *d, Query **out) override
            {
                auto *q = new Query12;
                q->desc = *d;
                q->heap = d->Query == D3D11_QUERY_OCCLUSION ? &g_occ : d->Query == D3D11_QUERY_TIMESTAMP ? &g_time : nullptr;
                *out = q;
                return S_OK;
            }
            bool NoOverwriteOnBufferSRV() override { return true; }
            bool ConstantBufferOffsets() override { return true; }
            const char *Name() override { return "DX12"; }
#ifdef HELSARI_FFX
            std::string UpscalerName() override { return g_fsr.load() ? g_fsr.name : std::string(); }
#endif
            std::string Stats(uint32_t frames) override
            {
                const double f = frames ? frames : 1;
                char b[300];
                snprintf(b, sizeof b,
                         "; DX12 per frame: resources created %.1f (%.1f ms), pipeline states built %.1f (%.1f ms), uploads %.1f MB "
                         "(waiting for room %.1f ms), read-back waits %.1f ms, submissions %.1f",
                         st_creates / f, st_create_ns / 1e6 / f, st_psos / f, st_pso_ns / 1e6 / f, st_up_bytes / 1048576.0 / f,
                         st_up_wait_ns / 1e6 / f, st_map_wait_ns / 1e6 / f, st_submits / f);
                std::vector<std::pair<uint64_t, uint64_t>> top(st_what.begin(), st_what.end());
                std::sort(top.begin(), top.end(), [](auto &a, auto &b) { return a.second > b.second; });
                std::string what;
                static const char *const kinds[] = {"?", "texture", "cube", "target", "buffer"};
                for (size_t i = 0; i < top.size() && i < 5; i++)
                {
                    const uint64_t k = top[i].first;
                    char t[96];
                    snprintf(t, sizeof t, " %s %llux%llu f%llu x%.1f", kinds[k >> 60], (k >> 40) & 0xfffff, (k >> 20) & 0xfffff, k & 0xfffff,
                             top[i].second / f);
                    what += t;
                }
                st_what.clear();
                std::string out = b;
                if (!what.empty())
                    out += "; created per frame:" + what;
                st_create_ns = st_creates = st_pso_ns = st_psos = st_up_bytes = st_up_wait_ns = st_map_wait_ns = st_submits = 0;
                return out;
            }
        };

        // ------------------------------------------------------------------ context
        template <class T> struct Bound
        {
            T *p = nullptr;
            void set(T *v)
            {
                if (v == p)
                    return;
                if (v)
                    v->AddRef();
                if (p)
                    p->Release();
                p = v;
            }
            ~Bound() { set(nullptr); }
        };

        struct Context12 final : Context
        {
            Bound<VertexShader> vs;
            Bound<PixelShader> ps;
            Bound<SRV> ps_srv[16], vs_srv[1];
            Bound<SamplerState> smp[16];
            // A constant buffer binding: the window [first, first + num) in 16-byte constants (num 0:
            // the whole buffer). Bound through a sized view, so reads outside it give 0 as in D3D11.
            struct Cb
            {
                Bound<Buffer> buf;
                UINT first = 0, num = 0;
            } vs_cb[3], ps_cb[1];
            Bound<Buffer> ib;
            DXGI_FORMAT ib_format = DXGI_FORMAT_R32_UINT;
            UINT ib_offset = 0;
            D3D11_PRIMITIVE_TOPOLOGY topo = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
            Bound<RasterizerState> raster;
            Bound<BlendState> blend;
            Bound<DepthStencilState> depth;
            FLOAT blend_factor[4] = {1, 1, 1, 1};
            UINT stencil_ref = 0;
            Bound<RTV> rtv[8];
            Bound<DSV> dsv;
            UINT num_rtv = 0;
            D3D11_VIEWPORT vp[16]{};
            UINT num_vp = 0;
            D3D11_RECT sc[16]{};
            UINT num_sc = 0;
            uint64_t gen = ~0ull;   // the submission count when state was last set on the list
            bool dirty_srv = true, dirty_vs_srv = true, dirty_smp = true;
            std::unordered_map<Key<16>, ID3D12PipelineState *, KeyHash> psos;
            std::vector<D3D12_RESOURCE_BARRIER> bars;

            // ---- resources
            HRESULT Map(Resource *r, UINT sub, D3D11_MAP type, UINT flags, D3D11_MAPPED_SUBRESOURCE *out) override
            {
                if (r->kind == Resource::BUFFER)
                {
                    auto *b = static_cast<Buf12 *>(r);
                    if (type == D3D11_MAP_WRITE_DISCARD && !b->new_version())
                        return E_OUTOFMEMORY;
                    out->pData = b->cur.ptr, out->RowPitch = out->DepthPitch = b->desc.ByteWidth;
                    if (type == D3D11_MAP_WRITE_DISCARD)
                        dirty_vs_srv = true;   // a view of it now points at the new version
                    return S_OK;
                }
                auto *t = static_cast<Tex12 *>(r);
                if (!t->staging)
                    return E_INVALIDARG;
                if (!g.done(t->written))
                {
                    if (flags & D3D11_MAP_FLAG_DO_NOT_WAIT)
                    {
                        if (t->written == g.next)
                            g.submit();   // let the copy start now
                        return DXGI_ERROR_WAS_STILL_DRAWING;
                    }
                    if (t->written == g.next)
                        g.submit();
                    const uint64_t t0 = now_ns();
                    g.wait(t->written);
                    st_map_wait_ns += now_ns() - t0;
                }
                const auto &f = t->fp[sub];
                out->pData = t->mapped + f.Offset;
                out->RowPitch = f.Footprint.RowPitch;
                out->DepthPitch = f.Footprint.RowPitch * t->rows[sub];
                return S_OK;
            }
            void Unmap(Resource *, UINT) override {}
            void UpdateSubresource(Resource *r, UINT sub, const D3D11_BOX *box, const void *data, UINT rp, UINT) override
            {
                if (r->kind == Resource::BUFFER)
                {
                    auto *b = static_cast<Buf12 *>(r);
                    b->new_version();
                    memcpy(b->cur.ptr + (box ? box->left : 0), data, box ? box->right - box->left : b->desc.ByteWidth);
                    return;
                }
                upload_subresource(static_cast<Tex12 *>(r), sub, box, data, rp);
            }
            // A copy into a staging texture lands at its footprints.
            void copy_to_staging(Tex12 *dst, UINT dsub, UINT x, UINT y, Tex12 *src, UINT ssub, const D3D11_BOX *box)
            {
                barrier(bars, src, D3D12_RESOURCE_STATE_COPY_SOURCE);
                flush_barriers(bars);
                D3D12_TEXTURE_COPY_LOCATION d{dst->res, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
                d.PlacedFootprint = dst->fp[dsub];
                D3D12_TEXTURE_COPY_LOCATION s{src->res, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
                s.SubresourceIndex = ssub;
                g.list->CopyTextureRegion(&d, x, y, 0, &s, (const D3D12_BOX *)box);
                g.recorded = true;
                dst->written = g.next;
            }
            void CopyResource(Resource *dst, Resource *src) override
            {
                Tex12 *d = tex_of(dst), *s = tex_of(src);
                if (!d || !s)
                    return;
                if (d->staging)
                {
                    for (UINT k = 0; k < (UINT)d->fp.size(); k++)
                        copy_to_staging(d, k, 0, 0, s, k, nullptr);
                    return;
                }
                barrier(bars, s, D3D12_RESOURCE_STATE_COPY_SOURCE);
                barrier(bars, d, D3D12_RESOURCE_STATE_COPY_DEST);
                flush_barriers(bars);
                g.list->CopyResource(d->res, s->res);
                g.recorded = true;
            }
            void CopySubresourceRegion(Resource *dst, UINT ds, UINT x, UINT y, UINT, Resource *src, UINT ss, const D3D11_BOX *box) override
            {
                Tex12 *d = tex_of(dst), *s = tex_of(src);
                if (!d || !s)
                    return;
                if (d->staging)
                {
                    copy_to_staging(d, ds, x, y, s, ss, box);
                    return;
                }
                barrier(bars, s, D3D12_RESOURCE_STATE_COPY_SOURCE);
                barrier(bars, d, D3D12_RESOURCE_STATE_COPY_DEST);
                flush_barriers(bars);
                D3D12_TEXTURE_COPY_LOCATION dl{d->res, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX}, sl{s->res, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
                dl.SubresourceIndex = ds, sl.SubresourceIndex = ss;
                g.list->CopyTextureRegion(&dl, x, y, 0, &sl, (const D3D12_BOX *)box);
                g.recorded = true;
            }

            // ---- bindings
            void VSSetShader(VertexShader *s, void *, UINT) override { vs.set(s); }
            void PSSetShader(PixelShader *s, void *, UINT) override { ps.set(s); }
            void VSSetShaderResources(UINT s, UINT n, SRV *const *v) override
            {
                for (UINT i = 0; i < n && s + i < 1; i++)
                {
                    SRV *x = v ? v[i] : nullptr;
                    dirty_vs_srv |= vs_srv[s + i].p != x;
                    vs_srv[s + i].set(x);
                }
            }
            void PSSetShaderResources(UINT s, UINT n, SRV *const *v) override
            {
                for (UINT i = 0; i < n && s + i < 16; i++)
                {
                    SRV *x = v ? v[i] : nullptr;
                    dirty_srv |= ps_srv[s + i].p != x;
                    ps_srv[s + i].set(x);
                }
            }
            void PSSetSamplers(UINT s, UINT n, SamplerState *const *v) override
            {
                for (UINT i = 0; i < n && s + i < 16; i++)
                {
                    SamplerState *x = v ? v[i] : nullptr;
                    dirty_smp |= smp[s + i].p != x;
                    smp[s + i].set(x);
                }
            }
            void VSSetConstantBuffers(UINT s, UINT n, Buffer *const *v) override
            {
                for (UINT i = 0; i < n && s + i < 3; i++)
                    vs_cb[s + i].buf.set(v[i]), vs_cb[s + i].first = 0, vs_cb[s + i].num = 0;
            }
            void PSSetConstantBuffers(UINT s, UINT n, Buffer *const *v) override
            {
                for (UINT i = 0; i < n && s + i < 1; i++)
                    ps_cb[s + i].buf.set(v[i]), ps_cb[s + i].first = 0, ps_cb[s + i].num = 0;
            }
            void VSSetConstantBuffers1(UINT s, UINT n, Buffer *const *v, const UINT *first, const UINT *num) override
            {
                for (UINT i = 0; i < n && s + i < 3; i++)
                    vs_cb[s + i].buf.set(v[i]), vs_cb[s + i].first = first[i], vs_cb[s + i].num = num[i];
            }
            void PSSetConstantBuffers1(UINT s, UINT n, Buffer *const *v, const UINT *first, const UINT *num) override
            {
                for (UINT i = 0; i < n && s + i < 1; i++)
                    ps_cb[s + i].buf.set(v[i]), ps_cb[s + i].first = first[i], ps_cb[s + i].num = num[i];
            }
            void IASetIndexBuffer(Buffer *b, DXGI_FORMAT f, UINT o) override { ib.set(b), ib_format = f, ib_offset = o; }
            void IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY t) override { topo = t; }
            void RSSetState(RasterizerState *s) override { raster.set(s); }
            void RSSetViewports(UINT n, const D3D11_VIEWPORT *v) override
            {
                num_vp = std::min(n, 16u);
                memcpy(vp, v, num_vp * sizeof *v);
            }
            void RSGetViewports(UINT *n, D3D11_VIEWPORT *v) override
            {
                const UINT k = std::min(*n, num_vp);
                memcpy(v, vp, k * sizeof *v);
                *n = num_vp;
            }
            void RSSetScissorRects(UINT n, const D3D11_RECT *r) override
            {
                num_sc = std::min(n, 16u);
                memcpy(sc, r, num_sc * sizeof *r);
            }
            void OMSetRenderTargets(UINT n, RTV *const *r, DSV *d) override
            {
                num_rtv = 0;
                for (UINT i = 0; i < 8; i++)
                {
                    RTV *v = i < n && r ? r[i] : nullptr;
                    rtv[i].set(v);
                    if (v)
                        num_rtv = i + 1;
                }
                dsv.set(d);
                dirty_srv = dirty_vs_srv = true;   // a target is not readable as a texture at the same time
            }
            void OMGetRenderTargets(UINT n, RTV **r, DSV **d) override
            {
                for (UINT i = 0; i < n; i++)
                    if ((r[i] = rtv[i].p))
                        r[i]->AddRef();
                if (d && (*d = dsv.p))
                    (*d)->AddRef();
            }
            void OMSetBlendState(BlendState *s, const FLOAT f[4], UINT) override
            {
                blend.set(s);
                const FLOAT one[4] = {1, 1, 1, 1};
                memcpy(blend_factor, f ? f : one, sizeof blend_factor);
            }
            void OMSetDepthStencilState(DepthStencilState *s, UINT ref) override { depth.set(s), stencil_ref = ref; }

            // ---- pipeline state
            ID3D12PipelineState *pso(bool has_ps)
            {
                // States are keyed by object: the renderer caches its state objects and keeps them.
                const D3D12_PRIMITIVE_TOPOLOGY_TYPE tt = topo == D3D11_PRIMITIVE_TOPOLOGY_POINTLIST ? D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT
                                                         : topo <= D3D11_PRIMITIVE_TOPOLOGY_LINESTRIP ? D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE
                                                                                                      : D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
                DXGI_FORMAT rf[8] = {};
                for (UINT i = 0; i < num_rtv; i++)
                    rf[i] = rtv[i].p ? rtv[i].p->format : DXGI_FORMAT_R8G8B8A8_UNORM;   // a gap: the null view's format
                const DXGI_FORMAT df = dsv.p ? dsv.p->format : DXGI_FORMAT_UNKNOWN;
                Key<16> key;
                key.v[0] = (uint64_t)(uintptr_t)vs.p, key.v[1] = (uint64_t)(uintptr_t)(has_ps ? ps.p : nullptr);
                key.v[2] = (uint64_t)(uintptr_t)blend.p, key.v[3] = (uint64_t)(uintptr_t)depth.p, key.v[4] = (uint64_t)(uintptr_t)raster.p;
                for (UINT i = 0; i < 8; i++)
                    key.v[5 + i] = rf[i];
                key.v[13] = df, key.v[14] = num_rtv, key.v[15] = tt;
                auto &p = psos[key];
                if (p)
                    return p;
                // D3D11's defaults stand in for unset states.
                D3D11_BLEND_DESC bd;
                memset(&bd, 0, sizeof bd);
                for (auto &t : bd.RenderTarget)
                    t.SrcBlend = t.SrcBlendAlpha = D3D11_BLEND_ONE, t.DestBlend = t.DestBlendAlpha = D3D11_BLEND_ZERO,
                    t.BlendOp = t.BlendOpAlpha = D3D11_BLEND_OP_ADD, t.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
                if (blend.p)
                    bd = blend.p->desc;
                D3D11_DEPTH_STENCIL_DESC dd;
                memset(&dd, 0, sizeof dd);
                dd.DepthEnable = TRUE, dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL, dd.DepthFunc = D3D11_COMPARISON_LESS;
                dd.StencilReadMask = dd.StencilWriteMask = 0xff;
                dd.FrontFace = dd.BackFace = {D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_KEEP, D3D11_COMPARISON_ALWAYS};
                if (depth.p)
                    dd = depth.p->desc;
                if (!dsv.p)
                    dd.DepthEnable = FALSE, dd.StencilEnable = FALSE, dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
                D3D11_RASTERIZER_DESC rd{D3D11_FILL_SOLID, D3D11_CULL_BACK, FALSE, 0, 0, 0, TRUE, FALSE, FALSE, FALSE};
                if (raster.p)
                    rd = raster.p->desc;
                D3D12_GRAPHICS_PIPELINE_STATE_DESC d{};
                d.pRootSignature = g.root;
                auto *v = static_cast<VS12 *>(vs.p);
                d.VS = {v->code.data(), v->code.size()};
                if (has_ps)
                {
                    auto *f = static_cast<PS12 *>(ps.p);
                    d.PS = {f->code.data(), f->code.size()};
                }
                d.BlendState.AlphaToCoverageEnable = bd.AlphaToCoverageEnable;
                d.BlendState.IndependentBlendEnable = bd.IndependentBlendEnable;
                for (int i = 0; i < 8; i++)
                {
                    const auto &s = bd.RenderTarget[i];
                    auto &t = d.BlendState.RenderTarget[i];
                    t.BlendEnable = s.BlendEnable, t.LogicOpEnable = FALSE;
                    t.SrcBlend = (D3D12_BLEND)s.SrcBlend, t.DestBlend = (D3D12_BLEND)s.DestBlend, t.BlendOp = (D3D12_BLEND_OP)s.BlendOp;
                    t.SrcBlendAlpha = (D3D12_BLEND)s.SrcBlendAlpha, t.DestBlendAlpha = (D3D12_BLEND)s.DestBlendAlpha;
                    t.BlendOpAlpha = (D3D12_BLEND_OP)s.BlendOpAlpha, t.LogicOp = D3D12_LOGIC_OP_NOOP;
                    t.RenderTargetWriteMask = s.RenderTargetWriteMask;
                }
                d.SampleMask = 0xffffffff;
                d.RasterizerState = {(D3D12_FILL_MODE)rd.FillMode, (D3D12_CULL_MODE)rd.CullMode, rd.FrontCounterClockwise, rd.DepthBias,
                                     rd.DepthBiasClamp, rd.SlopeScaledDepthBias, rd.DepthClipEnable, FALSE, FALSE, 0,
                                     D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF};
                memcpy(&d.DepthStencilState, &dd, sizeof dd);   // same layout and values
                d.PrimitiveTopologyType = tt;
                d.NumRenderTargets = num_rtv;
                memcpy(d.RTVFormats, rf, sizeof rf);
                d.DSVFormat = df;
                d.SampleDesc = {1, 0};
                const uint64_t t0 = now_ns();
                HRESULT hr = g.dev->CreateGraphicsPipelineState(&d, __uuidof(ID3D12PipelineState), (void **)&p);
                st_pso_ns += now_ns() - t0, st_psos++;
                if (FAILED(hr))
                {
                    static int logs;
                    if (logs++ < 20)
                        LOG("rsx", "DX12: pipeline state failed (0x%08lx), %u targets, depth format %u", hr, num_rtv, df);
                    p = nullptr;
                }
                return p;
            }

            // Everything a draw needs on the command list.
            bool prepare(bool has_ps)
            {
                if (!vs.p)
                    return false;
                ID3D12PipelineState *p = pso(has_ps);
                if (!p)
                    return false;
                const auto is_target = [&](Resource *r) {
                    for (UINT i = 0; i < num_rtv; i++)
                        if (rtv[i].p && rtv[i].p->resource == r)
                            return true;
                    return dsv.p && dsv.p->resource == r;
                };
                // Descriptor space first: making room can submit the list, and then everything is set
                // again on the new one (with space taken after that submission).
                uint64_t srv_base = 0, vs_base = 0, cb_base = 0;
                D3D12_GPU_DESCRIPTOR_HANDLE smp_table{};
                for (;;)
                {
                    const uint64_t before = g.gen;
                    cb_base = g_view_heap.ring.alloc(4, 1);   // constants change with nearly every draw
                    if (gen != g.gen)
                        dirty_srv = dirty_vs_srv = dirty_smp = true;
                    if (dirty_srv)
                        srv_base = g_view_heap.ring.alloc(16, 1);
                    if (dirty_vs_srv)
                        vs_base = g_view_heap.ring.alloc(1, 1);
                    if (dirty_smp)
                        smp_table = sampler_table();
                    if (g.gen == before)
                        break;
                }
                ID3D12GraphicsCommandList *l = g.list;
                if (gen != g.gen)
                {
                    l->SetGraphicsRootSignature(g.root);
                    ID3D12DescriptorHeap *heaps[] = {g_view_heap.heap, g_smp_heap};
                    l->SetDescriptorHeaps(2, heaps);
                    gen = g.gen;
                }
                // Targets, and every bound texture in a readable state (a bound target reads as unbound, as in D3D11).
                D3D12_CPU_DESCRIPTOR_HANDLE rh[8];
                for (UINT i = 0; i < num_rtv; i++)
                {
                    if (rtv[i].p)
                        barrier(bars, tex_of(rtv[i].p->resource), D3D12_RESOURCE_STATE_RENDER_TARGET),
                            rh[i] = g_rtv_cpu.cpu(static_cast<RTV12 *>(rtv[i].p)->slot);
                    else
                        rh[i] = g_rtv_cpu.cpu(0);   // the null view
                }
                D3D12_CPU_DESCRIPTOR_HANDLE dh{};
                if (dsv.p)
                    barrier(bars, tex_of(dsv.p->resource), D3D12_RESOURCE_STATE_DEPTH_WRITE), dh = g_dsv_cpu.cpu(static_cast<DSV12 *>(dsv.p)->slot);
                for (auto &v : ps_srv)
                    if (v.p && !is_target(v.p->resource))
                        barrier(bars, tex_of(v.p->resource), SRV_STATE);
                if (dirty_srv)
                {
                    for (UINT i = 0; i < 16; i++)
                    {
                        SRV *v = ps_srv[i].p;
                        if (v && !is_target(v->resource))
                            write_srv(static_cast<SRV12 *>(v), g_view_heap.cpu(srv_base + i));
                        else
                            g.dev->CopyDescriptorsSimple(1, g_view_heap.cpu(srv_base + i), g_null_srv, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
                    }
                    l->SetGraphicsRootDescriptorTable(2, g_view_heap.gpu(srv_base));
                    dirty_srv = false;
                }
                if (dirty_vs_srv)
                {
                    if (SRV *v = vs_srv[0].p)
                        write_srv(static_cast<SRV12 *>(v), g_view_heap.cpu(vs_base));
                    else
                        g.dev->CopyDescriptorsSimple(1, g_view_heap.cpu(vs_base), g_null_srv, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
                    l->SetGraphicsRootDescriptorTable(3, g_view_heap.gpu(vs_base));
                    dirty_vs_srv = false;
                }
                if (dirty_smp)
                {
                    l->SetGraphicsRootDescriptorTable(4, smp_table);
                    dirty_smp = false;
                }
                flush_barriers(bars);
                l->OMSetRenderTargets(num_rtv, num_rtv ? rh : nullptr, FALSE, dsv.p ? &dh : nullptr);
                l->SetPipelineState(p);
                for (int i = 0; i < 3; i++)
                    write_cbv(vs_cb[i], g_view_heap.cpu(cb_base + i));
                write_cbv(ps_cb[0], g_view_heap.cpu(cb_base + 3));
                l->SetGraphicsRootDescriptorTable(0, g_view_heap.gpu(cb_base));
                l->SetGraphicsRootDescriptorTable(1, g_view_heap.gpu(cb_base + 3));
                l->IASetPrimitiveTopology((D3D_PRIMITIVE_TOPOLOGY)topo);
                l->RSSetViewports(num_vp, (const D3D12_VIEWPORT *)vp);
                // D3D12 always scissors: without the D3D11 scissor test, the scissor covers everything.
                const bool scissor = raster.p && raster.p->desc.ScissorEnable;
                if (scissor && num_sc)
                    l->RSSetScissorRects(num_sc, (const D3D12_RECT *)sc);
                else
                {
                    D3D12_RECT all{0, 0, 16384, 16384};
                    l->RSSetScissorRects(1, &all);
                }
                l->OMSetBlendFactor(blend_factor);
                l->OMSetStencilRef(stencil_ref);
                g.recorded = true;
                return true;
            }
            void write_cbv(const Cb &c, D3D12_CPU_DESCRIPTOR_HANDLE to)
            {
                D3D12_CONSTANT_BUFFER_VIEW_DESC d{g_zero_cb->GetGPUVirtualAddress(), 256};
                if (auto *b = static_cast<Buf12 *>(c.buf.p))
                {
                    const UINT whole = std::min(65536u, (b->desc.ByteWidth + 255) & ~255u);
                    d.BufferLocation = b->va() + (uint64_t)c.first * 16;
                    d.SizeInBytes = c.num ? std::min(65536u, c.num * 16) : whole;
                }
                g.dev->CreateConstantBufferView(&d, to);
            }
            void write_srv(SRV12 *v, D3D12_CPU_DESCRIPTOR_HANDLE to)
            {
                if (v->resource->kind == Resource::BUFFER)
                    g.dev->CreateShaderResourceView(static_cast<Buf12 *>(v->resource)->cur.res, &v->d12, to);
                else
                    g.dev->CopyDescriptorsSimple(1, to, g_srv_cpu.cpu(v->slot), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
            }
            D3D12_GPU_DESCRIPTOR_HANDLE sampler_table()
            {
                Key<16> key;
                for (int i = 0; i < 16; i++)
                    key.v[i] = smp[i].p ? static_cast<Sampler12 *>(smp[i].p)->id : 0;
                auto it = g_smp_tables.find(key);
                if (it == g_smp_tables.end())
                {
                    if (g_smp_top + 16 > SAMPLER_HEAP)
                    {
                        g.idle();   // full (rare): start over once nothing uses the old tables
                        g_smp_tables.clear(), g_smp_top = 0;
                    }
                    const UINT inc = g.smp_size;
                    for (UINT i = 0; i < 16; i++)
                    {
                        D3D12_SAMPLER_DESC d{D3D12_FILTER_MIN_MAG_MIP_POINT, D3D12_TEXTURE_ADDRESS_MODE_CLAMP, D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
                                             D3D12_TEXTURE_ADDRESS_MODE_CLAMP, 0, 1, D3D12_COMPARISON_FUNC_NEVER, {0, 0, 0, 0}, 0, D3D12_FLOAT32_MAX};
                        if (smp[i].p)
                            memcpy(&d, &smp[i].p->desc, sizeof d);   // same layout and values
                        d.MaxAnisotropy = std::max(1u, d.MaxAnisotropy);
                        g.dev->CreateSampler(&d, {g_smp_heap->GetCPUDescriptorHandleForHeapStart().ptr + (SIZE_T)(g_smp_top + i) * inc});
                    }
                    it = g_smp_tables.emplace(key, g_smp_top).first;
                    g_smp_top += 16;
                }
                return {g_smp_heap->GetGPUDescriptorHandleForHeapStart().ptr + (UINT64)it->second * g.smp_size};
            }

            void Draw(UINT n, UINT s) override
            {
                if (prepare(ps.p != nullptr))
                    g.list->DrawInstanced(n, 1, s, 0);
            }
            void DrawIndexed(UINT n, UINT s, INT b) override
            {
                if (!ib.p || !prepare(ps.p != nullptr))
                    return;
                auto *buf = static_cast<Buf12 *>(ib.p);
                D3D12_INDEX_BUFFER_VIEW v{buf->va() + ib_offset, buf->desc.ByteWidth - ib_offset, ib_format};
                g.list->IASetIndexBuffer(&v);
                g.list->DrawIndexedInstanced(n, 1, s, b, 0);
            }
            void ClearRenderTargetView(RTV *v, const FLOAT c[4]) override
            {
                barrier(bars, tex_of(v->resource), D3D12_RESOURCE_STATE_RENDER_TARGET);
                flush_barriers(bars);
                g.list->ClearRenderTargetView(g_rtv_cpu.cpu(static_cast<RTV12 *>(v)->slot), c, 0, nullptr);
                g.recorded = true;
            }
            void ClearDepthStencilView(DSV *v, UINT f, FLOAT d, UINT8 s) override
            {
                barrier(bars, tex_of(v->resource), D3D12_RESOURCE_STATE_DEPTH_WRITE);
                flush_barriers(bars);
                D3D12_CLEAR_FLAGS cf = (D3D12_CLEAR_FLAGS)((f & D3D11_CLEAR_DEPTH ? D3D12_CLEAR_FLAG_DEPTH : 0) |
                                                           (f & D3D11_CLEAR_STENCIL ? D3D12_CLEAR_FLAG_STENCIL : 0));
                g.list->ClearDepthStencilView(g_dsv_cpu.cpu(static_cast<DSV12 *>(v)->slot), cf, d, s, 0, nullptr);
                g.recorded = true;
            }

            // ---- queries
            // A query's slots are reused once it begins again (its old results were read or abandoned).
            void reset_slots(Query12 *x)
            {
                for (UINT i : x->slots)
                    x->heap->free.push_back(i);
                x->slots.assign(1, x->heap->take());
                x->fence = 0;
            }
            void Begin(Query *q) override
            {
                auto *x = static_cast<Query12 *>(q);
                if (x->heap != &g_occ)
                    return;
                reset_slots(x);
                g.list->BeginQuery(g_occ.heap, D3D12_QUERY_TYPE_OCCLUSION, x->slots.back());
                g.recorded = true;
                g.active_occ = x;
            }
            void End(Query *q) override
            {
                auto *x = static_cast<Query12 *>(q);
                if (!x->heap)
                    return;
                if (x->heap == &g_time)
                    reset_slots(x);
                if (x->slots.empty())
                    return;
                const D3D12_QUERY_TYPE t = x->heap == &g_occ ? D3D12_QUERY_TYPE_OCCLUSION : D3D12_QUERY_TYPE_TIMESTAMP;
                g.list->EndQuery(x->heap->heap, t, x->slots.back());
                g.list->ResolveQueryData(x->heap->heap, t, x->slots.back(), 1, x->heap->results, (UINT64)x->slots.back() * 8);
                g.recorded = true;
                x->fence = g.next;
                if (g.active_occ == x)
                    g.active_occ = nullptr;
            }
            HRESULT GetData(Query *q, void *data, UINT size, UINT flags) override
            {
                auto *x = static_cast<Query12 *>(q);
                if (q->desc.Query == D3D11_QUERY_TIMESTAMP_DISJOINT)
                {
                    D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj{};
                    g.queue->GetTimestampFrequency(&dj.Frequency);
                    if (data)
                        memcpy(data, &dj, std::min<size_t>(size, sizeof dj));
                    return S_OK;
                }
                if (!x->fence)
                    return S_FALSE;
                if (!g.done(x->fence))
                {
                    if (x->fence == g.next && !(flags & D3D11_ASYNC_GETDATA_DONOTFLUSH))
                        g.submit();
                    return S_FALSE;
                }
                uint64_t v = 0;
                for (UINT i : x->slots)
                    v = x->heap == &g_occ ? v + x->heap->mapped[i] : x->heap->mapped[i];
                if (data)
                    memcpy(data, &v, std::min<size_t>(size, 8));
                return S_OK;
            }
            void Flush() override { g.submit(); }
#ifdef HELSARI_FFX
            bool Upscale(const UpscaleParams &u) override
            {
                Tex12 *color = tex_of(u.color), *depth = tex_of(u.depth), *motion = tex_of(u.motion), *out = tex_of(u.output);
                if (!color || !depth || !motion || !out || !g_fsr.ensure(u.render_w, u.render_h, u.out_w, u.out_h, u.inverted_depth))
                    return false;
                barrier(bars, color, SRV_STATE);
                barrier(bars, depth, SRV_STATE);
                barrier(bars, motion, SRV_STATE);
                barrier(bars, out, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                flush_barriers(bars);
                ffxDispatchDescUpscale d{};
                d.header.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
                d.commandList = g.list;
                d.color = ffxApiGetResourceDX12(color->res, FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ);
                d.depth = ffxApiGetResourceDX12(depth->res, FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ);
                d.motionVectors = ffxApiGetResourceDX12(motion->res, FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ);
                d.output = ffxApiGetResourceDX12(out->res, FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);
                d.jitterOffset = {u.jitter_x, u.jitter_y};
                d.motionVectorScale = {u.mv_scale_x, u.mv_scale_y};
                d.renderSize = {u.render_w, u.render_h};
                d.upscaleSize = {u.out_w, u.out_h};
                d.enableSharpening = u.sharpness > 0;
                d.sharpness = u.sharpness;
                d.frameTimeDelta = u.frame_ms;
                d.preExposure = 1.f;
                d.reset = u.reset;
                d.cameraNear = u.near_z, d.cameraFar = u.far_z, d.cameraFovAngleVertical = u.fov_y;
                d.viewSpaceToMetersFactor = 1.f;
                const ffxReturnCode_t rc = g_fsr.dispatch(&g_fsr.ctx, &d.header);
                g.recorded = true;
                g.gen++;   // FSR set its own pipeline, root signature and heaps on our list
                if (rc != FFX_API_RETURN_OK)
                {
                    static int logs;
                    if (logs++ < 10)
                        LOG("rsx", "FSR: dispatch failed (%u)", rc);
                    return false;
                }
                return true;
            }
#endif
        };
        Context12 *g_ctx;

        // ------------------------------------------------------------------ swapchain
        struct Swap12 final : Swap
        {
            static constexpr UINT BUFFERS = 3;
            IDXGISwapChain3 *s = nullptr;
            Device12 *dev = nullptr;
            Tex12 *buf[BUFFERS] = {};
            RTV *rtv[BUFFERS] = {};
            bool tearing = false;
            uint64_t frame_fence[BUFFERS] = {};
            UINT frame = 0;
            bool wrap()
            {
                for (UINT i = 0; i < BUFFERS; i++)
                {
                    auto *t = new Tex12;
                    t->swapchain = true;
                    if (FAILED(s->GetBuffer(i, __uuidof(ID3D12Resource), (void **)&t->res)))
                        return t->Release(), false;
                    D3D12_RESOURCE_DESC rd = t->res->GetDesc();
                    t->desc = {(UINT)rd.Width, rd.Height, 1, 1, rd.Format, {1, 0}, D3D11_USAGE_DEFAULT, D3D11_BIND_RENDER_TARGET, 0, 0};
                    t->state = D3D12_RESOURCE_STATE_PRESENT;
                    buf[i] = t;
                    dev->CreateRenderTargetView(t, nullptr, &rtv[i]);
                }
                return true;
            }
            void unwrap()
            {
                for (UINT i = 0; i < BUFFERS; i++)
                    release(rtv[i]), release(buf[i]);
            }
            RTV *Backbuffer() override { return rtv[s->GetCurrentBackBufferIndex()]; }
            void Resize(UINT w, UINT h) override
            {
                g.idle();
                unwrap();
                HRESULT hr = s->ResizeBuffers(BUFFERS, w, h, DXGI_FORMAT_R8G8B8A8_UNORM, tearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0);
                if (FAILED(hr))
                    LOG("rsx", "DX12: swapchain resize to %ux%u failed (0x%08lx)", w, h, hr);
                wrap();
            }
            void Present(bool vsync) override
            {
                Tex12 *t = buf[s->GetCurrentBackBufferIndex()];
                std::vector<D3D12_RESOURCE_BARRIER> b;
                barrier(b, t, D3D12_RESOURCE_STATE_PRESENT);
                flush_barriers(b);
                g_ctx->Flush();
                s->Present(vsync ? 1 : 0, !vsync && tearing ? DXGI_PRESENT_ALLOW_TEARING : 0);
                // At most two frames ahead of the GPU (HELSARI_DX12_FRAMES=1..3).
                static const UINT ahead = [] {
                    const char *e = getenv("HELSARI_DX12_FRAMES");
                    const int v = e ? atoi(e) : 2;
                    return (UINT)(v < 1 ? 1 : v > (int)BUFFERS ? BUFFERS : v);
                }();
                frame_fence[frame % BUFFERS] = g.next - 1;
                frame++;
                g.wait(frame_fence[(frame + BUFFERS - ahead) % BUFFERS]);
                g.collect();
            }
        };
    } // namespace

    bool create_d3d12(IDXGIAdapter1 *adapter, HWND hwnd, bool debug, Device **dev, Context **ctx, Swap **swap)
    {
        if (debug)
        {
            ID3D12Debug *dbg = nullptr;
            if (SUCCEEDED(D3D12GetDebugInterface(__uuidof(ID3D12Debug), (void **)&dbg)))
                dbg->EnableDebugLayer(), dbg->Release();
        }
        HRESULT hr = D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_11_0, __uuidof(ID3D12Device), (void **)&g.dev);
        if (FAILED(hr))
        {
            LOG("rsx", "DX12: device creation failed (0x%08lx)", hr);
            return false;
        }
        D3D12_COMMAND_QUEUE_DESC qd{};
        qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        g.dev->CreateCommandQueue(&qd, __uuidof(ID3D12CommandQueue), (void **)&g.queue);
        g.dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, __uuidof(ID3D12Fence), (void **)&g.fence);
        g.fence_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        g.dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, __uuidof(ID3D12CommandAllocator), (void **)&g.alloc);
        g.dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g.alloc, nullptr, __uuidof(ID3D12GraphicsCommandList), (void **)&g.list);
        g.smp_size = g.dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);

        // Root signature, all descriptor tables: VS constants b0-b2, PS constants b0 (sized views: reads
        // outside a window give 0), PS textures t0-t15, VS vertex ring t0, PS samplers s0-s15.
        D3D12_DESCRIPTOR_RANGE ranges[5] = {{D3D12_DESCRIPTOR_RANGE_TYPE_CBV, 3, 0, 0, 0},
                                            {D3D12_DESCRIPTOR_RANGE_TYPE_CBV, 1, 0, 0, 0},
                                            {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 16, 0, 0, 0},
                                            {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0},
                                            {D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER, 16, 0, 0, 0}};
        const D3D12_SHADER_VISIBILITY vis[5] = {D3D12_SHADER_VISIBILITY_VERTEX, D3D12_SHADER_VISIBILITY_PIXEL, D3D12_SHADER_VISIBILITY_PIXEL,
                                                D3D12_SHADER_VISIBILITY_VERTEX, D3D12_SHADER_VISIBILITY_PIXEL};
        D3D12_ROOT_PARAMETER params[5] = {};
        for (int i = 0; i < 5; i++)
        {
            params[i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            params[i].DescriptorTable = {1, &ranges[i]};
            params[i].ShaderVisibility = vis[i];
        }
        D3D12_ROOT_SIGNATURE_DESC rs{5, params, 0, nullptr,
                                     D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS | D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS |
                                         D3D12_ROOT_SIGNATURE_FLAG_DENY_GEOMETRY_SHADER_ROOT_ACCESS};
        ID3DBlob *blob = nullptr, *err = nullptr;
        if (FAILED(D3D12SerializeRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err)) ||
            FAILED(g.dev->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), __uuidof(ID3D12RootSignature), (void **)&g.root)))
        {
            LOG("rsx", "DX12: root signature failed: %s", err ? (const char *)err->GetBufferPointer() : "?");
            return false;
        }
        release(blob), release(err);

        g_srv_cpu.init(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 1 << 18);
        g_rtv_cpu.init(D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1 << 14);
        g_dsv_cpu.init(D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 1 << 12);
        {
            // Slot 0: a null view for unbound targets between bound ones.
            D3D12_RENDER_TARGET_VIEW_DESC nd{DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RTV_DIMENSION_TEXTURE2D};
            g.dev->CreateRenderTargetView(nullptr, &nd, g_rtv_cpu.cpu(g_rtv_cpu.alloc()));
        }
        {
            D3D12_DESCRIPTOR_HEAP_DESC d{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 1 << 20, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
            g.dev->CreateDescriptorHeap(&d, __uuidof(ID3D12DescriptorHeap), (void **)&g_view_heap.heap);
            g_view_heap.inc = g.dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
            g_view_heap.ring.size = 1 << 20;
            D3D12_DESCRIPTOR_HEAP_DESC sd{D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, SAMPLER_HEAP, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
            g.dev->CreateDescriptorHeap(&sd, __uuidof(ID3D12DescriptorHeap), (void **)&g_smp_heap);
        }
        // An empty view for unbound texture slots.
        {
            const UINT slot = g_srv_cpu.alloc();
            g_null_srv = g_srv_cpu.cpu(slot);
            D3D12_SHADER_RESOURCE_VIEW_DESC s{DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_SRV_DIMENSION_TEXTURE2D, D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING};
            s.Texture2D.MipLevels = 1;
            g.dev->CreateShaderResourceView(nullptr, &s, g_null_srv);
        }
        // Upload ring, zero constant buffer, query heaps.
        {
            constexpr uint64_t UPLOAD = 256ull << 20;
            D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_UPLOAD};
            D3D12_RESOURCE_DESC rd{D3D12_RESOURCE_DIMENSION_BUFFER, 0, UPLOAD, 1, 1, 1, DXGI_FORMAT_UNKNOWN, {1, 0}, D3D12_TEXTURE_LAYOUT_ROW_MAJOR,
                                   D3D12_RESOURCE_FLAG_NONE};
            g.dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, __uuidof(ID3D12Resource),
                                           (void **)&g_up.res);
            D3D12_RANGE none{0, 0};
            g_up.res->Map(0, &none, (void **)&g_up.ptr);
            g_up.ring.size = UPLOAD;
            rd.Width = 256;
            g.dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, __uuidof(ID3D12Resource),
                                           (void **)&g_zero_cb);
            void *z = nullptr;
            g_zero_cb->Map(0, &none, &z);
            memset(z, 0, 256);
        }
        g_occ.init(D3D12_QUERY_HEAP_TYPE_OCCLUSION, QUERY_SLOTS);
        g_time.init(D3D12_QUERY_HEAP_TYPE_TIMESTAMP, QUERY_SLOTS);

        // Flip-model swapchain; with tearing allowed, V-sync off is not held to the refresh rate.
        auto *d = new Device12;
        auto *sw = new Swap12;
        sw->dev = d;
        IDXGIFactory5 *factory = nullptr;
        if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory5), (void **)&factory)))
            return false;
        BOOL tearing = FALSE;
        factory->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &tearing, sizeof tearing);
        sw->tearing = tearing != FALSE;
        DXGI_SWAP_CHAIN_DESC1 sd{};
        sd.Width = 1280, sd.Height = 720, sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        sd.SampleDesc.Count = 1;
        sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        sd.BufferCount = Swap12::BUFFERS;
        sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        sd.Flags = sw->tearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;
        IDXGISwapChain1 *s1 = nullptr;
        hr = factory->CreateSwapChainForHwnd(g.queue, hwnd, &sd, nullptr, nullptr, &s1);
        factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);
        factory->Release();
        if (FAILED(hr) || FAILED(s1->QueryInterface(__uuidof(IDXGISwapChain3), (void **)&sw->s)))
        {
            LOG("rsx", "DX12: swapchain creation failed (0x%08lx)", hr);
            release(s1);
            return false;
        }
        s1->Release();
        if (!sw->wrap())
            return false;
        g_ctx = new Context12;
        *dev = d, *ctx = g_ctx, *swap = sw;
#ifdef HELSARI_FFX
        g_fsr.load();   // logs the FSR versions this card offers
#endif
        LOG("rsx", "DX12: device, flip-model swapchain (%u buffers, tearing %s)", Swap12::BUFFERS, sw->tearing ? "allowed" : "not supported");
        return true;
    }
} // namespace gpu
