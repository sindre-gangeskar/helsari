// cellAdec: movie audio.
//
// Same shape as cellVdec: the game hands over one access unit (one AC-3
// frame) per cellAdecDecodeAu, a guest thread "decodes" it and reports
// through the game's callback (AUDONE, then PCMOUT), and the game reads the
// samples with GetPcmItem / GetPcm.
//
// AC-3 frames are decoded by a Windows Media Foundation decoder when the
// system has one that outputs PCM, else by our own decoder (ac3.cpp); recent
// Windows 11 builds ship only an S/PDIF pass-through converter. Output is
// stereo (5.1 mixed down). Decoded samples go through a queue, so every frame
// still yields exactly 1536 samples and the movie clock stays exact even if a
// decoder holds frames back.
#include "runtime.h"
#include "ac3.h"

#include <windows.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mftransform.h>

#include <algorithm>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <vector>

#define REG(n) hle_register(#n, hle_##n)

namespace {
constexpr int32_t ADEC_ERROR_SEQ = (int32_t)0x80610002;
constexpr int32_t ADEC_ERROR_ARG = (int32_t)0x80610003;
constexpr int32_t ADEC_ERROR_EMPTY = (int32_t)0x80610005;
enum : uint32_t { MSG_AUDONE = 0, MSG_PCMOUT = 1, MSG_ERROR = 2, MSG_SEQDONE = 3 };

constexpr uint32_t FRAME_SAMPLES = 1536;   // per AC-3 frame
constexpr uint32_t MAX_CHANNELS = 8;
constexpr uint32_t SLOT_SIZE = FRAME_SAMPLES * MAX_CHANNELS * 4;   // float samples
constexpr uint32_t SLOTS = 16;
constexpr uint32_t ITEM_SIZE = 0x30, BSI_SIZE = 0x40;   // the item, then its codec info block

struct Au { uint32_t addr, size, pts_hi, pts_lo; uint64_t user; };
struct Pcm { uint32_t slot, size; Au au; };

enum class Cmd { Au, EndSeq, Close };
struct Job { Cmd cmd; Au au{}; std::vector<uint8_t> data; };

struct Decoder {
    uint32_t id = 0, cb = 0, cb_arg = 0, type = 0, channels = 2;
    uint32_t pcm_ea = 0, item_ea = 0, next_slot = 0;
    std::mutex lock;
    std::condition_variable cv;
    std::deque<Job> jobs;
    std::deque<Pcm> pcm;
    bool seq = false;

    // Decoder thread only:
    IMFTransform *mft = nullptr;
    ac3::Decoder *own = nullptr;   // when Windows has no AC-3 decoder
    bool mft_tried = false, provides_samples = false;
    uint32_t out_channels = 0, out_size = 0;
    bool out_float = true;   // else 16-bit integer PCM
    int64_t t = 0;
    std::deque<float> fifo;   // decoded stereo samples, interleaved
};

std::mutex g_lock;
std::map<uint32_t, Decoder *> g_decoders;
uint32_t g_next_id = 0x100;

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

void push(Decoder *d, Job j) {
    std::lock_guard<std::mutex> lk(d->lock);
    d->jobs.push_back(std::move(j));
    d->cv.notify_one();
}

template <class T> void release(T *&p) { if (p) p->Release(), p = nullptr; }

// Channels of an AC-3 frame, from its header (audio coding mode and LFE flag).
uint32_t ac3_channels(const std::vector<uint8_t> &f) {
    if (f.size() < 8 || f[0] != 0x0b || f[1] != 0x77) return 0;
    const uint32_t bits = (uint32_t)f[6] << 8 | f[7];   // acmod is the first field of byte 6
    uint32_t pos = 0;
    auto take = [&](uint32_t n) { const uint32_t v = bits >> (16 - pos - n) & ((1u << n) - 1); pos += n; return v; };
    const uint32_t acmod = take(3);
    if ((acmod & 1) && acmod != 1) take(2);   // centre mix level
    if (acmod & 4) take(2);                   // surround mix level
    if (acmod == 2) take(2);                  // Dolby Surround mode
    static const uint32_t full[8] = {2, 1, 2, 3, 3, 4, 4, 5};
    return full[acmod] + take(1);             // + LFE
}

// Sets up one candidate transform: AC-3 in, stereo (else downmixed here) float or 16-bit PCM out.
// Some installed transforms only repackage AC-3 for S/PDIF pass-through; those offer no PCM.
bool try_mft(Decoder *d, IMFTransform *m, uint32_t channels) {
    IMFMediaType *t = nullptr;
    MFCreateMediaType(&t);
    t->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    t->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_Dolby_AC3);
    t->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, 48000);
    t->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, channels);
    HRESULT hr = m->SetInputType(0, t, 0);
    t->Release();
    if (FAILED(hr)) { LOG("adec", "  AC-3 input refused (0x%08lx)", hr); return false; }
    IMFMediaType *pick = nullptr;
    for (DWORD i = 0;; i++) {
        IMFMediaType *o = nullptr;
        if (FAILED(m->GetOutputAvailableType(0, i, &o))) break;
        GUID sub{};
        UINT32 ch = 0, bits = 0;
        o->GetGUID(MF_MT_SUBTYPE, &sub);
        o->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &ch);
        o->GetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, &bits);
        const bool usable = sub == MFAudioFormat_Float || (sub == MFAudioFormat_PCM && bits == 16);
        if (usable && (!pick || ch == 2)) {
            release(pick);
            pick = o, d->out_channels = ch, d->out_float = sub == MFAudioFormat_Float;
            if (ch == 2) break;
        } else
            o->Release();
    }
    if (!pick) { LOG("adec", "  no PCM output (pass-through only)"); return false; }
    hr = m->SetOutputType(0, pick, 0);
    release(pick);
    if (FAILED(hr)) { LOG("adec", "  output type refused (0x%08lx)", hr); return false; }
    MFT_OUTPUT_STREAM_INFO si{};
    m->GetOutputStreamInfo(0, &si);
    d->provides_samples = si.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES | MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES);
    d->out_size = std::max<DWORD>(si.cbSize, FRAME_SAMPLES * MAX_CHANNELS * 4);
    m->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    m->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
    return true;
}

bool open_mft(Decoder *d, uint32_t channels) {
    MFT_REGISTER_TYPE_INFO in{MFMediaType_Audio, MFAudioFormat_Dolby_AC3};
    IMFActivate **act = nullptr;
    UINT32 n = 0;
    if (FAILED(MFTEnumEx(MFT_CATEGORY_AUDIO_DECODER, MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_SORTANDFILTER, &in, nullptr,
                         &act, &n)))
        n = 0;
    for (UINT32 i = 0; i < n; i++) {
        WCHAR *wname = nullptr;
        UINT32 wlen = 0;
        std::string name = "?";
        if (SUCCEEDED(act[i]->GetAllocatedString(MFT_FRIENDLY_NAME_Attribute, &wname, &wlen))) {
            name.assign(wname, wname + wlen);   // the names are plain ASCII
            CoTaskMemFree(wname);
        }
        if (!d->mft) {
            IMFTransform *m = nullptr;
            LOG("adec", "decoder 0x%x: trying '%s'", d->id, name.c_str());
            if (SUCCEEDED(act[i]->ActivateObject(IID_PPV_ARGS(&m))) && try_mft(d, m, channels))
                d->mft = m;
            else if (m)
                act[i]->ShutdownObject(), m->Release();
        }
        act[i]->Release();
    }
    CoTaskMemFree(act);
    if (!d->mft) { LOG("adec", "decoder 0x%x: no AC-3 decoder in Windows, using our own", d->id); return false; }
    LOG("adec", "decoder 0x%x: AC-3 %u channels -> %s, %u channels out", d->id, channels, d->out_float ? "float" : "16-bit",
        d->out_channels);
    return true;
}

// Append decoded samples to the queue as stereo.
void take_samples(Decoder *d, IMFSample *s) {
    IMFMediaBuffer *b = nullptr;
    if (FAILED(s->ConvertToContiguousBuffer(&b))) return;
    BYTE *p = nullptr;
    DWORD len = 0;
    if (SUCCEEDED(b->Lock(&p, nullptr, &len))) {
        const uint32_t ch = d->out_channels, bytes = d->out_float ? 4 : 2, frames = ch ? len / bytes / ch : 0;
        std::vector<float> conv;
        if (!d->out_float) {   // 16-bit integer to float
            conv.resize((size_t)frames * ch);
            const int16_t *in = (const int16_t *)p;
            for (size_t k = 0; k < conv.size(); k++) conv[k] = in[k] / 32768.0f;
        }
        const float *f = d->out_float ? (const float *)p : conv.data();
        for (uint32_t i = 0; i < frames; i++, f += ch) {
            float l = f[0], r = ch > 1 ? f[1] : f[0];
            if (ch >= 6) {   // FL FR FC LFE BL BR: fold the centre and surrounds in
                l += 0.707f * (f[2] + f[4]), r += 0.707f * (f[2] + f[5]);
                l *= 0.586f, r *= 0.586f;   // 1 / (1 + 2 * 0.707)
            }
            d->fifo.push_back(l), d->fifo.push_back(r);
        }
        b->Unlock();
    }
    b->Release();
}

void decode(Decoder *d, const std::vector<uint8_t> &frame) {
    if (!d->mft_tried) {
        d->mft_tried = true;
        if (const uint32_t ch = ac3_channels(frame)) {
            if (!open_mft(d, ch)) d->own = new ac3::Decoder;
        } else
            LOG("adec", "decoder 0x%x: not an AC-3 frame, audio stays silent", d->id);
    }
    if (d->own) {
        static float pcm[ac3::FRAME_SAMPLES * 2];
        if (!d->own->decode(frame.data(), frame.size(), pcm)) {
            static int logged;
            if (logged++ < 5) LOG("adec", "AC-3 frame not decoded: %s", d->own->error() ? d->own->error() : "?");
        }
        d->fifo.insert(d->fifo.end(), pcm, pcm + ac3::FRAME_SAMPLES * 2);
        return;
    }
    if (!d->mft || frame.empty()) return;
    IMFMediaBuffer *mb = nullptr;
    IMFSample *s = nullptr;
    MFCreateMemoryBuffer((DWORD)frame.size(), &mb);
    BYTE *dst = nullptr;
    mb->Lock(&dst, nullptr, nullptr);
    memcpy(dst, frame.data(), frame.size());
    mb->Unlock();
    mb->SetCurrentLength((DWORD)frame.size());
    MFCreateSample(&s);
    s->AddBuffer(mb);
    mb->Release();
    s->SetSampleTime(d->t);
    s->SetSampleDuration(320000);   // 1536 samples at 48 kHz, in 100 ns units
    d->t += 320000;
    HRESULT hr = d->mft->ProcessInput(0, s, 0);
    s->Release();
    if (FAILED(hr)) {
        static int logged;
        if (logged++ < 3) LOG("adec", "ProcessInput failed 0x%08lx", hr);
        return;
    }
    for (;;) {
        MFT_OUTPUT_DATA_BUFFER ob{};
        IMFSample *own = nullptr;
        if (!d->provides_samples) {
            IMFMediaBuffer *buf = nullptr;
            MFCreateSample(&own);
            MFCreateMemoryBuffer(d->out_size, &buf);
            own->AddBuffer(buf);
            buf->Release();
            ob.pSample = own;
        }
        DWORD status = 0;
        hr = d->mft->ProcessOutput(0, 1, &ob, &status);
        if (ob.pEvents) ob.pEvents->Release();
        if (FAILED(hr)) { release(own); break; }   // MF_E_TRANSFORM_NEED_MORE_INPUT: all out
        take_samples(d, ob.pSample);
        if (ob.pSample != own) release(ob.pSample);
        release(own);
    }
}

// The decoder's guest thread: r3 = decoder id.
void adec_thread(PPUContext *ctx) {
    Decoder *d = find(ARG32(0));
    if (!d) return;
    name_host_thread("adec");
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    MFStartup(MF_VERSION, MFSTARTUP_LITE);
    for (;;) {
        Job j;
        {
            std::unique_lock<std::mutex> lk(d->lock);
            d->cv.wait(lk, [&] { return !d->jobs.empty(); });
            j = std::move(d->jobs.front());
            d->jobs.pop_front();
        }
        if (j.cmd == Cmd::Close) break;
        if (j.cmd == Cmd::EndSeq) {
            if (d->mft) d->mft->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
            d->fifo.clear();
            callback(ctx, d, MSG_SEQDONE, 0);
            continue;
        }
        callback(ctx, d, MSG_AUDONE, 0);   // the frame's bytes were copied when it was queued
        decode(d, j.data);
        // Exactly one frame's worth out per frame in, as big-endian floats.
        Pcm p{d->next_slot, FRAME_SAMPLES * d->channels * 4, j.au};
        d->next_slot = (d->next_slot + 1) % SLOTS;
        uint32_t *out = (uint32_t *)mem::ptr(d->pcm_ea + p.slot * SLOT_SIZE);
        const size_t n = (size_t)FRAME_SAMPLES * d->channels, have = std::min((size_t)FRAME_SAMPLES * 2, d->fifo.size());
        for (size_t i = 0; i < n; i++) {
            const size_t frame = i / d->channels, c = i % d->channels, src = frame * 2 + c;
            const float v = c < 2 && src < have ? d->fifo[src] : 0.0f;
            uint32_t u;
            memcpy(&u, &v, 4);
            out[i] = __builtin_bswap32(u);
        }
        d->fifo.erase(d->fifo.begin(), d->fifo.begin() + (ptrdiff_t)have);
        {
            std::lock_guard<std::mutex> lk(d->lock);
            d->pcm.push_back(p);
        }
        callback(ctx, d, MSG_PCMOUT, 0);
    }
    release(d->mft);
    delete d->own;
    MFShutdown();
    CoUninitialize();
    std::lock_guard<std::mutex> lk(g_lock);
    g_decoders.erase(d->id);
    delete d;
}

} // namespace

HLE(cellAdecQueryAttr) {
    // (CellAdecType *type, CellAdecAttr *attr); attr: u32 workMemSize, u32 verUpper, u32 verLower
    LOG("adec", "QueryAttr type %u", ld32(ARG32(0)));
    uint32_t attr = ARG32(1);
    st32(attr + 0, 0x10000);
    st32(attr + 4, 0x00010000);
    st32(attr + 8, 0);
    RET(0);
}

HLE(cellAdecOpenEx) {
    // (CellAdecType *type, CellAdecResourceEx *res, CellAdecCb *cb, u32 *handle)
    auto *d = new Decoder;
    d->type = ld32(ARG32(0));
    d->cb = ld32(ARG32(2)), d->cb_arg = ld32(ARG32(2) + 4);
    d->pcm_ea = mem::alloc(SLOT_SIZE * SLOTS, 128);
    d->item_ea = mem::alloc(ITEM_SIZE + BSI_SIZE, 16);
    {
        std::lock_guard<std::mutex> lk(g_lock);
        d->id = g_next_id++;
        g_decoders[d->id] = d;
    }
    static const uint32_t entry = hle_make_opd(adec_thread);
    thread::create(entry, d->id, 1000, 0x10000, "adec", false);
    st32(ARG32(3), d->id);
    LOG("adec", "decoder 0x%x open: type %u (callback 0x%x)", d->id, d->type, d->cb);
    RET(0);
}

HLE(cellAdecClose) {
    Decoder *d = find(ARG32(0));
    LOG("adec", "Close 0x%x%s", ARG32(0), d ? "" : " (unknown handle)");
    if (!d) { RET(ADEC_ERROR_ARG); return; }
    push(d, Job{Cmd::Close});
    RET(0);
}

HLE(cellAdecStartSeq) {
    // (handle, void *param): the parameter block depends on the codec.
    Decoder *d = find(ARG32(0));
    LOG("adec", "StartSeq 0x%x%s", ARG32(0), d ? "" : " (unknown handle)");
    if (!d) { RET(ADEC_ERROR_ARG); return; }
    if (uint32_t prm = ARG32(1))
        LOG("adec", "StartSeq param %08x %08x %08x %08x", ld32(prm), ld32(prm + 4), ld32(prm + 8), ld32(prm + 12));
    std::lock_guard<std::mutex> lk(d->lock);
    d->seq = true;
    d->pcm.clear();
    RET(0);
}

HLE(cellAdecEndSeq) {
    Decoder *d = find(ARG32(0));
    LOG("adec", "EndSeq 0x%x%s", ARG32(0), d ? "" : " (unknown handle)");
    if (!d) { RET(ADEC_ERROR_ARG); return; }
    {
        std::lock_guard<std::mutex> lk(d->lock);
        if (!d->seq) { RET(ADEC_ERROR_SEQ); return; }
        d->seq = false;
    }
    push(d, Job{Cmd::EndSeq});
    RET(0);
}

HLE(cellAdecDecodeAu) {
    // (handle, CellAdecAuInfo *au): u32 startAddr, u32 size, pts {u32 upper, lower}, u64 userData
    Decoder *d = find(ARG32(0));
    if (!d) { RET(ADEC_ERROR_ARG); return; }
    uint32_t a = ARG32(1);
    Job j{Cmd::Au, {ld32(a), ld32(a + 4), ld32(a + 8), ld32(a + 12), ld64(a + 16)}};
    j.data.assign((const uint8_t *)mem::ptr(j.au.addr), (const uint8_t *)mem::ptr(j.au.addr) + j.au.size);
    static int logged;
    if (logged < 3 && ++logged) LOG("adec", "DecodeAu: %u bytes at 0x%x, pts %x:%08x", j.au.size, j.au.addr, j.au.pts_hi,
                                    j.au.pts_lo);
    push(d, std::move(j));
    RET(0);
}

HLE(cellAdecGetPcmItem) {
    // (handle, CellAdecPcmItem **item): describes the oldest block without taking it.
    // CellAdecPcmItem: u32 pcmHandle, u32 status, u32 startAddr, u32 size,
    //   pcmAttr {u32 bsiInfo: pointer to codec info}, (pad),
    //   auInfo at +24 {u32 startAddr, u32 size, pts {upper, lower}, u64 userData}
    Decoder *d = find(ARG32(0));
    if (!d) { RET(ADEC_ERROR_ARG); return; }
    std::lock_guard<std::mutex> lk(d->lock);
    if (d->pcm.empty()) { RET(ADEC_ERROR_EMPTY); return; }
    const Pcm &p = d->pcm.front();
    uint32_t it = d->item_ea, bsi = d->item_ea + ITEM_SIZE;
    memset(mem::ptr(it), 0, ITEM_SIZE + BSI_SIZE);
    st32(it + 0, p.slot);
    st32(it + 4, 0);
    st32(it + 8, d->pcm_ea + p.slot * SLOT_SIZE);
    st32(it + 12, p.size);
    st32(it + 16, bsi);
    st32(it + 24, p.au.addr), st32(it + 28, p.au.size);
    st32(it + 32, p.au.pts_hi), st32(it + 36, p.au.pts_lo);
    st64(it + 40, p.au.user);
    // Codec info. The game checks the output size at +8 on one path and at
    // +20 on the other; channels and rate lead, as in the LPCM layout.
    st32(bsi + 0, d->channels);
    st32(bsi + 4, 48000);
    st32(bsi + 8, p.size);
    st32(bsi + 20, p.size);
    st32(ARG32(1), it);
    RET(0);
}

HLE(cellAdecGetPcm) {
    // (handle, float *out): takes the oldest block; out == 0 drops it.
    Decoder *d = find(ARG32(0));
    if (!d) { RET(ADEC_ERROR_ARG); return; }
    Pcm p;
    {
        std::lock_guard<std::mutex> lk(d->lock);
        if (d->pcm.empty()) { RET(ADEC_ERROR_EMPTY); return; }
        p = d->pcm.front();
        d->pcm.pop_front();
    }
    if (uint32_t out = ARG32(1)) memcpy(mem::ptr(out), mem::ptr(d->pcm_ea + p.slot * SLOT_SIZE), p.size);
    RET(0);
}

void register_adec() {
    REG(cellAdecQueryAttr); REG(cellAdecOpenEx); REG(cellAdecClose); REG(cellAdecStartSeq);
    REG(cellAdecEndSeq); REG(cellAdecDecodeAu); REG(cellAdecGetPcmItem); REG(cellAdecGetPcm);
}
