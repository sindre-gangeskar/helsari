// cellVdec: H.264 video decoding with the Windows Media Foundation decoder.
//
// The game demuxes its movies itself and hands us one access unit (AU, the
// bytes of one coded picture) per cellVdecDecodeAu call. Each decoder runs
// a guest thread that feeds AUs to the decoder, keeps the decoded pictures
// in a queue and reports through the game's callback:
//   AUDONE  the AU's bytes are no longer needed
//   PICOUT  a picture is ready (read it with GetPicItem / GetPicture)
//   SEQDONE EndSeq finished: every remaining picture has been output
// Pictures come out of the decoder as NV12 and are converted to the format
// the game asks for in cellVdecGetPicture.
#include "runtime.h"

#include <windows.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mftransform.h>

#include <algorithm>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <vector>

#define REG(n) hle_register(#n, hle_##n)

namespace {
constexpr int32_t VDEC_ERROR_ARG = (int32_t)0x80610101;
constexpr int32_t VDEC_ERROR_SEQ = (int32_t)0x80610102;
constexpr int32_t VDEC_ERROR_EMPTY = (int32_t)0x80610104;
constexpr int32_t VDEC_ERROR_FATAL = (int32_t)0x80610180;
enum : uint32_t { MSG_AUDONE = 0, MSG_PICOUT = 1, MSG_SEQDONE = 2, MSG_ERROR = 3 };
enum : uint32_t { FMT_ARGB32 = 0, FMT_RGBA32 = 1, FMT_UYVY422 = 2, FMT_YUV420 = 3 };
constexpr uint32_t CODEC_AVC = 1;

// Guest-side structures we fill in (offsets from the public PicItem layout).
constexpr uint32_t ITEM_SIZE = 0x50, AVC_INFO_SIZE = 0x140;

template <class T> void release(T *&p) { if (p) p->Release(), p = nullptr; }

struct AuInfo { uint32_t pts_hi, pts_lo, dts_hi, dts_lo; uint64_t user; };

struct Picture {
    uint32_t w = 0, h = 0;
    std::vector<uint8_t> nv12;   // w*h luma, then w*h/2 interleaved chroma
    AuInfo au{};
};

enum class Cmd { Au, EndSeq, Close };
struct Job { Cmd cmd; uint32_t addr = 0, size = 0; AuInfo au{}; };

struct Decoder {
    uint32_t id = 0, cb = 0, cb_arg = 0, codec = 0;
    uint32_t item_ea = 0, info_ea = 0;   // our guest memory for GetPicItem
    std::mutex lock;
    std::condition_variable cv;
    std::deque<Job> jobs;
    std::deque<Picture> pics;
    bool seq = false;

    // Decoder thread only:
    IMFTransform *mft = nullptr;
    uint32_t out_w = 0, out_h = 0, out_stride = 0, out_rows = 0;
    bool provides_samples = false;
    uint32_t out_size = 0;
    uint64_t au_count = 0;
    std::map<int64_t, AuInfo> in_flight;   // sample time -> AU info
};

std::mutex g_lock;
std::map<uint32_t, Decoder *> g_decoders;
uint32_t g_next_id = 1;

Decoder *find(uint32_t id) {
    std::lock_guard<std::mutex> lk(g_lock);
    auto it = g_decoders.find(id);
    return it == g_decoders.end() ? nullptr : it->second;
}

void callback(PPUContext *ctx, Decoder *d, uint32_t msg, int32_t data) {
    ctx->r[3] = d->id;
    ctx->r[4] = msg;
    ctx->r[5] = (uint64_t)(int64_t)data;
    ctx->r[6] = d->cb_arg;
    ppu_invoke(ctx, d->cb);
}

/* ------------------------------ Media Foundation ------------------------------ */

IMFTransform *create_h264() {
    MFT_REGISTER_TYPE_INFO in{MFMediaType_Video, MFVideoFormat_H264};
    MFT_REGISTER_TYPE_INFO out{MFMediaType_Video, MFVideoFormat_NV12};
    IMFActivate **act = nullptr;
    UINT32 n = 0;
    // Synchronous decoders only: they take input and give output on our thread.
    if (FAILED(MFTEnumEx(MFT_CATEGORY_VIDEO_DECODER, MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_SORTANDFILTER, &in, &out,
                         &act, &n)))
        return nullptr;
    IMFTransform *mft = nullptr;
    for (UINT32 i = 0; i < n; i++) {
        if (!mft && FAILED(act[i]->ActivateObject(IID_PPV_ARGS(&mft)))) mft = nullptr;
        act[i]->Release();
    }
    CoTaskMemFree(act);
    return mft;
}

bool set_output_type(Decoder *d) {
    for (DWORD i = 0;; i++) {
        IMFMediaType *t = nullptr;
        if (FAILED(d->mft->GetOutputAvailableType(0, i, &t))) return false;
        GUID sub{};
        t->GetGUID(MF_MT_SUBTYPE, &sub);
        if (sub == MFVideoFormat_NV12 && SUCCEEDED(d->mft->SetOutputType(0, t, 0))) {
            UINT32 w = 0, h = 0, stride = 0;
            MFGetAttributeSize(t, MF_MT_FRAME_SIZE, &w, &h);
            d->out_stride = SUCCEEDED(t->GetUINT32(MF_MT_DEFAULT_STRIDE, &stride)) ? stride : w;
            d->out_rows = h;   // rows per plane in the buffer (may be padded, e.g. 1088)
            // The visible picture can be smaller than the coded frame.
            MFVideoArea area{};
            if (SUCCEEDED(t->GetBlob(MF_MT_MINIMUM_DISPLAY_APERTURE, (UINT8 *)&area, sizeof area, nullptr)) &&
                area.Area.cx && area.Area.cy)
                w = area.Area.cx, h = area.Area.cy;
            d->out_w = w, d->out_h = h;
            t->Release();
            MFT_OUTPUT_STREAM_INFO si{};
            d->mft->GetOutputStreamInfo(0, &si);
            d->provides_samples = si.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES | MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES);
            d->out_size = si.cbSize;
            LOG("vdec", "decoder %u output %ux%u NV12, stride %u, rows %u", d->id, d->out_w, d->out_h, d->out_stride,
                d->out_rows);
            return true;
        }
        t->Release();
    }
}

bool open_mft(Decoder *d) {
    d->mft = create_h264();
    if (!d->mft) { LOG("vdec", "no H.264 decoder found in Media Foundation"); return false; }
    // Out of the box the decoder holds several pictures back (to reorder
    // B-frames) before the first comes out, but the game waits for a picture
    // before it sends more data. Low-latency mode outputs each one as soon
    // as it can.
    IMFAttributes *attrs = nullptr;
    if (SUCCEEDED(d->mft->GetAttributes(&attrs))) {
        attrs->SetUINT32(MF_LOW_LATENCY, TRUE);
        attrs->Release();
    }
    IMFMediaType *t = nullptr;
    MFCreateMediaType(&t);
    t->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    t->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
    HRESULT hr = d->mft->SetInputType(0, t, 0);
    t->Release();
    if (FAILED(hr)) { LOG("vdec", "SetInputType failed 0x%08lx", hr); return false; }
    set_output_type(d);   // retried after the first STREAM_CHANGE
    d->mft->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    d->mft->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
    return true;
}

// Copy one decoded sample into the picture queue and tell the game.
void take_sample(PPUContext *ctx, Decoder *d, IMFSample *s) {
    Picture p;
    p.w = d->out_w, p.h = d->out_h;
    LONGLONG t = 0;
    s->GetSampleTime(&t);
    auto it = d->in_flight.find(t);
    if (it != d->in_flight.end()) p.au = it->second, d->in_flight.erase(it);
    p.nv12.resize((size_t)p.w * p.h * 3 / 2);

    IMFMediaBuffer *buf = nullptr;
    if (FAILED(s->ConvertToContiguousBuffer(&buf))) return;
    BYTE *src = nullptr;
    LONG pitch = (LONG)d->out_stride;
    IMF2DBuffer *b2 = nullptr;
    bool locked2d = SUCCEEDED(buf->QueryInterface(IID_PPV_ARGS(&b2))) && SUCCEEDED(b2->Lock2D(&src, &pitch));
    DWORD len = 0;
    if (!locked2d && FAILED(buf->Lock(&src, nullptr, &len))) src = nullptr;
    if (src) {
        const uint8_t *y = src, *uv = src + (size_t)pitch * d->out_rows;
        uint8_t *dy = p.nv12.data(), *duv = dy + (size_t)p.w * p.h;
        for (uint32_t r = 0; r < p.h; r++) memcpy(dy + (size_t)r * p.w, y + (size_t)r * pitch, p.w);
        for (uint32_t r = 0; r < p.h / 2; r++) memcpy(duv + (size_t)r * p.w, uv + (size_t)r * pitch, p.w);
        locked2d ? b2->Unlock2D() : buf->Unlock();
    }
    release(b2);
    release(buf);
    {
        std::lock_guard<std::mutex> lk(d->lock);
        d->pics.push_back(std::move(p));
    }
    static int logged;
    if (logged < 3 && ++logged) LOG("vdec", "decoder %u picture out (%ux%u)", d->id, d->out_w, d->out_h);
    callback(ctx, d, MSG_PICOUT, 0);
}

// Pull every picture the decoder has ready.
void drain(PPUContext *ctx, Decoder *d) {
    for (;;) {
        MFT_OUTPUT_DATA_BUFFER ob{};
        IMFSample *own = nullptr;
        if (!d->provides_samples) {
            IMFMediaBuffer *mb = nullptr;
            MFCreateSample(&own);
            MFCreateMemoryBuffer(std::max<DWORD>(d->out_size, d->out_stride * d->out_rows * 3 / 2), &mb);
            own->AddBuffer(mb);
            mb->Release();
            ob.pSample = own;
        }
        DWORD status = 0;
        HRESULT hr = d->mft->ProcessOutput(0, 1, &ob, &status);
        if (ob.pEvents) ob.pEvents->Release();
        if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
            release(own);
            if (!set_output_type(d)) { callback(ctx, d, MSG_ERROR, VDEC_ERROR_FATAL); return; }
            continue;
        }
        if (FAILED(hr)) {   // MF_E_TRANSFORM_NEED_MORE_INPUT: nothing more for now
            if (hr != MF_E_TRANSFORM_NEED_MORE_INPUT) LOG("vdec", "ProcessOutput failed 0x%08lx", hr);
            release(own);
            return;
        }
        take_sample(ctx, d, ob.pSample);
        if (ob.pSample != own) release(ob.pSample);
        release(own);
    }
}

void decode_au(PPUContext *ctx, Decoder *d, const Job &j) {
    IMFMediaBuffer *mb = nullptr;
    IMFSample *s = nullptr;
    MFCreateMemoryBuffer(j.size, &mb);
    BYTE *dst = nullptr;
    mb->Lock(&dst, nullptr, nullptr);
    memcpy(dst, mem::ptr(j.addr), j.size);
    mb->Unlock();
    mb->SetCurrentLength(j.size);
    MFCreateSample(&s);
    s->AddBuffer(mb);
    mb->Release();
    // Number AUs in decode order; the decoder hands each picture back with
    // its own AU's number, which finds the AU's timestamps again.
    const int64_t t = (int64_t)(d->au_count++) * 10000;
    s->SetSampleTime(t);
    s->SetSampleDuration(10000);
    d->in_flight[t] = j.au;
    // The game may reuse the AU's memory as soon as it hears AUDONE.
    callback(ctx, d, MSG_AUDONE, 0);
    HRESULT hr = d->mft->ProcessInput(0, s, 0);
    if (hr == MF_E_NOTACCEPTING) {   // output first, then retry
        drain(ctx, d);
        hr = d->mft->ProcessInput(0, s, 0);
    }
    s->Release();
    if (FAILED(hr)) LOG("vdec", "ProcessInput failed 0x%08lx (AU %u bytes)", hr, j.size);
    drain(ctx, d);
}

// The decoder's guest thread: r3 = decoder id.
void vdec_thread(PPUContext *ctx) {
    Decoder *d = find(ARG32(0));
    if (!d) return;
    name_host_thread("vdec");
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    MFStartup(MF_VERSION, MFSTARTUP_LITE);
    bool ok = open_mft(d);
    for (;;) {
        Job j;
        {
            std::unique_lock<std::mutex> lk(d->lock);
            d->cv.wait(lk, [&] { return !d->jobs.empty(); });
            j = d->jobs.front();
            d->jobs.pop_front();
        }
        if (j.cmd == Cmd::Close) break;
        if (j.cmd == Cmd::Au) {
            if (ok) decode_au(ctx, d, j);
            else callback(ctx, d, MSG_AUDONE, 0);
        } else {   // EndSeq: flush the decoder's held pictures, then reset for the next sequence
            if (ok) {
                d->mft->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
                d->mft->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0);
                drain(ctx, d);
                d->mft->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
                d->in_flight.clear();
            }
            callback(ctx, d, MSG_SEQDONE, 0);
        }
    }
    release(d->mft);
    MFShutdown();
    CoUninitialize();
    {
        std::lock_guard<std::mutex> lk(g_lock);
        g_decoders.erase(d->id);
    }
    delete d;
}

void push(Decoder *d, Job j) {
    std::lock_guard<std::mutex> lk(d->lock);
    d->jobs.push_back(j);
    d->cv.notify_one();
}

/* ------------------------------ colour conversion ------------------------------ */

inline uint8_t clamp8(int v) { return (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v); }

// Limited-range YCbCr to RGB, coefficients scaled by 1 << 16.
void to_rgb(const Picture &p, uint8_t *out, bool argb, uint8_t alpha, bool bt709) {
    const int cy = 76309;   // 255/219
    const int crv = bt709 ? 117489 : 104597, cgu = bt709 ? 13975 : 25675, cgv = bt709 ? 34925 : 53279,
              cbu = bt709 ? 138438 : 132201;
    const uint8_t *Y = p.nv12.data(), *UV = Y + (size_t)p.w * p.h;
    for (uint32_t r = 0; r < p.h; r++) {
        uint8_t *o = out + (size_t)r * p.w * 4;
        const uint8_t *uv = UV + (size_t)(r / 2) * p.w;
        for (uint32_t c = 0; c < p.w; c++, o += 4) {
            int y = (Y[(size_t)r * p.w + c] - 16) * cy;
            int u = uv[c & ~1u] - 128, v = uv[c | 1u] - 128;
            uint8_t R = clamp8((y + crv * v + 32768) >> 16);
            uint8_t G = clamp8((y - cgu * u - cgv * v + 32768) >> 16);
            uint8_t B = clamp8((y + cbu * u + 32768) >> 16);
            if (argb) o[0] = alpha, o[1] = R, o[2] = G, o[3] = B;
            else o[0] = R, o[1] = G, o[2] = B, o[3] = alpha;
        }
    }
}

void to_yuv420(const Picture &p, uint8_t *out) {
    const size_t n = (size_t)p.w * p.h;
    memcpy(out, p.nv12.data(), n);
    uint8_t *u = out + n, *v = u + n / 4;
    const uint8_t *uv = p.nv12.data() + n;
    for (size_t i = 0; i < n / 4; i++) u[i] = uv[2 * i], v[i] = uv[2 * i + 1];
}

void to_uyvy(const Picture &p, uint8_t *out) {
    const uint8_t *Y = p.nv12.data(), *UV = Y + (size_t)p.w * p.h;
    for (uint32_t r = 0; r < p.h; r++)
        for (uint32_t c = 0; c < p.w; c += 2) {
            const uint8_t *uv = UV + (size_t)(r / 2) * p.w + c;
            uint8_t *o = out + ((size_t)r * p.w + c) * 2;
            o[0] = uv[0], o[1] = Y[(size_t)r * p.w + c], o[2] = uv[1], o[3] = Y[(size_t)r * p.w + c + 1];
        }
}

void write_item(Decoder *d, const Picture &p) {
    uint32_t it = d->item_ea, info = d->info_ea;
    memset(mem::ptr(it), 0, ITEM_SIZE);
    memset(mem::ptr(info), 0, AVC_INFO_SIZE);
    st32(it + 0, d->codec);
    st32(it + 4, 0);   // startAddr / size of the AU are not kept
    st32(it + 8, 0);
    st8(it + 12, 1);   // auNum
    st32(it + 16, p.au.pts_hi), st32(it + 20, p.au.pts_lo);
    st32(it + 24, 0xffffffff), st32(it + 28, 0xffffffff);   // second AU: none
    st32(it + 32, p.au.dts_hi), st32(it + 36, p.au.dts_lo);
    st32(it + 40, 0xffffffff), st32(it + 44, 0xffffffff);
    st64(it + 48, p.au.user);
    st32(it + 64, 0);   // status: OK
    st32(it + 68, 0);   // attr
    st32(it + 72, info);
    // CellVdecAvcInfo: only what a player needs.
    st16(info + 0, (uint16_t)p.w);
    st16(info + 2, (uint16_t)p.h);
    st8(info + 7, 1);    // aspect_ratio_idc: square pixels
    st8(info + 19, 1);   // frame_mbs_only_flag: progressive
}
} // namespace

/* ------------------------------------ HLE ------------------------------------ */

HLE(cellVdecQueryAttrEx) {
    // CellVdecAttr: u32 memSize, u8 cmdDepth, u32 decoderVerUpper, u32 decoderVerLower
    uint32_t type = ARG32(0), attr = ARG32(1);
    LOG("vdec", "QueryAttrEx codec %u profile 0x%x", ld32(type), ld32(type + 4));
    st32(attr + 0, 0x100000);
    st8(attr + 4, 4);
    st32(attr + 8, 0x00010000);
    st32(attr + 12, 0);
    RET(0);
}

HLE(cellVdecOpenEx) {
    // (CellVdecTypeEx *type, CellVdecResourceEx *res, CellVdecCb *cb, u32 *handle)
    uint32_t type = ARG32(0), cb = ARG32(2);
    if (ld32(type) != CODEC_AVC) {
        LOG("vdec", "OpenEx: codec %u not supported", ld32(type));
        RET(VDEC_ERROR_ARG);
        return;
    }
    auto *d = new Decoder;
    d->codec = ld32(type);
    d->cb = ld32(cb), d->cb_arg = ld32(cb + 4);
    d->item_ea = mem::alloc(ITEM_SIZE, 16);
    d->info_ea = mem::alloc(AVC_INFO_SIZE, 16);
    {
        std::lock_guard<std::mutex> lk(g_lock);
        d->id = g_next_id++;
        g_decoders[d->id] = d;
    }
    static const uint32_t entry = hle_make_opd(vdec_thread);
    thread::create(entry, d->id, 1000, 0x10000, "vdec", false);
    st32(ARG32(3), d->id);
    LOG("vdec", "decoder %u open (callback 0x%x)", d->id, d->cb);
    RET(0);
}

HLE(cellVdecClose) {
    Decoder *d = find(ARG32(0));
    LOG("vdec", "Close 0x%x%s", ARG32(0), d ? "" : " (unknown handle)");
    if (!d) { RET(VDEC_ERROR_ARG); return; }
    push(d, {Cmd::Close});
    RET(0);
}

HLE(cellVdecStartSeq) {
    Decoder *d = find(ARG32(0));
    LOG("vdec", "StartSeq 0x%x%s", ARG32(0), d ? "" : " (unknown handle)");
    if (!d) { RET(VDEC_ERROR_ARG); return; }
    std::lock_guard<std::mutex> lk(d->lock);
    d->seq = true;
    d->pics.clear();
    RET(0);
}

HLE(cellVdecEndSeq) {
    Decoder *d = find(ARG32(0));
    LOG("vdec", "EndSeq 0x%x%s", ARG32(0), d ? "" : " (unknown handle)");
    if (!d) { RET(VDEC_ERROR_ARG); return; }
    {
        std::lock_guard<std::mutex> lk(d->lock);
        if (!d->seq) { RET(VDEC_ERROR_SEQ); return; }
        d->seq = false;
    }
    push(d, {Cmd::EndSeq});
    RET(0);
}

HLE(cellVdecDecodeAu) {
    // (handle, CellVdecDecodeMode mode, CellVdecAuInfo *au)
    // CellVdecAuInfo: u32 startAddr, u32 size, pts {u32 upper, lower}, dts, u64 userData, u64 codecSpecificData
    Decoder *d = find(ARG32(0));
    if (!d) { RET(VDEC_ERROR_ARG); return; }
    uint32_t a = ARG32(2);
    Job j{Cmd::Au, ld32(a), ld32(a + 4), {ld32(a + 8), ld32(a + 12), ld32(a + 16), ld32(a + 20), ld64(a + 24)}};
    static int logged;
    if (logged < 3 && ++logged) LOG("vdec", "DecodeAu mode %u: %u bytes at 0x%x, pts %x:%08x", ARG32(1), j.size, j.addr,
                                    j.au.pts_hi, j.au.pts_lo);
    push(d, j);
    RET(0);
}

HLE(cellVdecGetPicItem) {
    // (handle, CellVdecPicItem **item): describes the oldest picture without taking it.
    Decoder *d = find(ARG32(0));
    if (!d) { RET(VDEC_ERROR_ARG); return; }
    std::lock_guard<std::mutex> lk(d->lock);
    if (d->pics.empty()) { RET(VDEC_ERROR_EMPTY); return; }
    write_item(d, d->pics.front());
    st32(ARG32(1), d->item_ea);
    RET(0);
}

HLE(cellVdecGetPicture) {
    // (handle, CellVdecPicFormat *format, u8 *out); format: u32 type, u32 colorMatrix, u8 alpha.
    // out == 0 drops the picture.
    Decoder *d = find(ARG32(0));
    if (!d) { RET(VDEC_ERROR_ARG); return; }
    Picture p;
    {
        std::lock_guard<std::mutex> lk(d->lock);
        if (d->pics.empty()) { RET(VDEC_ERROR_EMPTY); return; }
        p = std::move(d->pics.front());
        d->pics.pop_front();
    }
    uint32_t f = ARG32(1), out = ARG32(2);
    if (out && f) {
        uint32_t type = ld32(f), matrix = ld32(f + 4);
        uint8_t alpha = *(uint8_t *)mem::ptr(f + 8);   // a single byte after the two words
        static int logged;
        if (logged < 1 && ++logged) LOG("vdec", "GetPicture format %u matrix %u alpha %u -> 0x%x (%ux%u)", type, matrix,
                                        alpha, out, p.w, p.h);
        uint8_t *o = (uint8_t *)mem::ptr(out);
        switch (type) {
        case FMT_ARGB32: to_rgb(p, o, true, alpha, matrix == 1); break;
        case FMT_RGBA32: to_rgb(p, o, false, alpha, matrix == 1); break;
        case FMT_UYVY422: to_uyvy(p, o); break;
        case FMT_YUV420: to_yuv420(p, o); break;
        default: LOG("vdec", "GetPicture: unknown format %u", type); break;
        }
    }
    RET(0);
}

HLE(cellVdecSetFrameRate) { RET(0); }   // pictures carry the game's own timestamps

void register_vdec() {
    REG(cellVdecQueryAttrEx); REG(cellVdecOpenEx); REG(cellVdecClose); REG(cellVdecStartSeq);
    REG(cellVdecEndSeq); REG(cellVdecDecodeAu); REG(cellVdecGetPicItem); REG(cellVdecGetPicture);
    REG(cellVdecSetFrameRate);
}
