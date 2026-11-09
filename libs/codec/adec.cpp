// cellAdec: movie audio.
//
// Same shape as cellVdec: the game hands over one access unit (one AC-3
// frame) per cellAdecDecodeAu, a guest thread "decodes" it and reports
// through the game's callback (AUDONE, then PCMOUT), and the game reads the
// samples with GetPcmItem / GetPcm.
//
// First step: every frame becomes the right amount of silence, so a movie's
// audio clock advances as it should. Real AC-3 decoding comes next.
#include "runtime.h"

#include <condition_variable>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>

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
struct Job { Cmd cmd; Au au{}; };

struct Decoder {
    uint32_t id = 0, cb = 0, cb_arg = 0, type = 0, channels = 2;
    uint32_t pcm_ea = 0, item_ea = 0, next_slot = 0;
    std::mutex lock;
    std::condition_variable cv;
    std::deque<Job> jobs;
    std::deque<Pcm> pcm;
    bool seq = false;
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
    d->jobs.push_back(j);
    d->cv.notify_one();
}

// The decoder's guest thread: r3 = decoder id.
void adec_thread(PPUContext *ctx) {
    Decoder *d = find(ARG32(0));
    if (!d) return;
    name_host_thread("adec");
    for (;;) {
        Job j;
        {
            std::unique_lock<std::mutex> lk(d->lock);
            d->cv.wait(lk, [&] { return !d->jobs.empty(); });
            j = d->jobs.front();
            d->jobs.pop_front();
        }
        if (j.cmd == Cmd::Close) break;
        if (j.cmd == Cmd::EndSeq) { callback(ctx, d, MSG_SEQDONE, 0); continue; }
        callback(ctx, d, MSG_AUDONE, 0);
        Pcm p{d->next_slot, FRAME_SAMPLES * d->channels * 4, j.au};
        d->next_slot = (d->next_slot + 1) % SLOTS;
        memset(mem::ptr(d->pcm_ea + p.slot * SLOT_SIZE), 0, p.size);
        {
            std::lock_guard<std::mutex> lk(d->lock);
            d->pcm.push_back(p);
        }
        callback(ctx, d, MSG_PCMOUT, 0);
    }
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
    push(d, {Cmd::Close});
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
    push(d, {Cmd::EndSeq});
    RET(0);
}

HLE(cellAdecDecodeAu) {
    // (handle, CellAdecAuInfo *au): u32 startAddr, u32 size, pts {u32 upper, lower}, u64 userData
    Decoder *d = find(ARG32(0));
    if (!d) { RET(ADEC_ERROR_ARG); return; }
    uint32_t a = ARG32(1);
    Job j{Cmd::Au, {ld32(a), ld32(a + 4), ld32(a + 8), ld32(a + 12), ld64(a + 16)}};
    static int logged;
    if (logged < 3 && ++logged) LOG("adec", "DecodeAu: %u bytes at 0x%x, pts %x:%08x", j.au.size, j.au.addr, j.au.pts_hi,
                                    j.au.pts_lo);
    push(d, j);
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
