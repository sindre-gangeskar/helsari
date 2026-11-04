// Mapping recompiled SPU modules (build/generated/spu) into a thread's LS.
// A module maps when its image is in LS at its base - copied there in one
// piece (code_loaded), or found when execution reaches it (map_at) - and
// unmaps when anything writes to one of its code quadwords. Native code exists
// only for code quadwords, so writes to the rest of the image (data) are harmless.
// EBOOT modules (task ELFs, job binaries) are compared with the EBOOT; runtime
// modules (captured by spu_capture.cpp) with their embedded image, on their
// code quadwords. KZ2_SPU_NATIVE=0 disables it.
#include "spu.h"

#include "runtime.h"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace spu {

extern const NativeModule g_native_modules[];      // from the EBOOT (spu_modules.cpp)
extern const int g_native_module_count;
extern const NativeModule g_rt_modules[];          // from spu_cache/ (spu_rt_modules.cpp)
extern const int g_rt_module_count;

namespace {
const bool g_enabled = [] {
    const char *e = getenv("KZ2_SPU_NATIVE");
    return !e || atoi(e) != 0;
}();

// guest source address -> modules compiled from it
const std::unordered_multimap<uint32_t, const NativeModule *> &by_source() {
    static const auto m = [] {
        std::unordered_multimap<uint32_t, const NativeModule *> r;
        for (int i = 0; i < g_native_module_count; i++) r.emplace(g_native_modules[i].src_ea, &g_native_modules[i]);
        return r;
    }();
    return m;
}

// KZ2_SPU_RT=0: ignore modules compiled from spu_cache/ (EBOOT modules still map).
const bool g_rt_enabled = [] {
    const char *e = getenv("KZ2_SPU_RT");
    return !e || atoi(e) != 0;
}();
} // namespace
std::atomic<uint64_t> g_maps{0}, g_unmaps{0}, g_match_tries{0};   // reported in the flip log (spu_interp.cpp)
namespace {

bool is_code(const NativeModule *m, uint32_t q) {   // q: quadword index from ls_base
    return !m->protect || m->protect[q];
}

// Does LS hold m's image? Runtime modules are compared on their code quadwords only:
// those are all that was compiled, and the rest is data the program may have changed.
bool image_matches(const uint8_t *ls, const NativeModule *m) {
    // EBOOT modules compare with the game image, captured ones with their embedded copy. Only code
    // counts: the rest of a segment is data the running program has changed, and comparing it
    // would keep a task that was unmapped once in the interpreter for good.
    const uint8_t *img = m->image ? m->image : (const uint8_t *)mem::ptr(m->src_ea);
    if (!m->protect) return !memcmp(ls + m->ls_base, img, m->size);
    // protect: per quadword, a bit per executed word (spu_capture.cpp); runs of whole
    // quadwords are compared with one memcmp, partial ones word by word.
    for (uint32_t off = 0; off < m->size;) {
        uint8_t bits = m->protect[off >> 4] & 0xf;
        if (bits != 0xf) {
            for (uint32_t w = 0; w < 4 && off + w * 4 < m->size; w++)
                if ((bits >> w & 1) && memcmp(ls + m->ls_base + off + w * 4, img + off + w * 4, 4)) return false;
            off += 16;
            continue;
        }
        uint32_t end = off;
        while (end < m->size && (m->protect[end >> 4] & 0xf) == 0xf) end += 16;
        end = std::min(end, m->size);
        if (memcmp(ls + m->ls_base + off, img + off, end - off)) return false;
        off = end;
    }
    return true;
}
} // namespace

bool SpuThread::map_module(const NativeModule *m) {
    int slot = -1;
    for (int i = 0; i < 8; i++) if (!mapped[i].m) { slot = i; break; }
    if (slot < 0) return false;
    if (!native) native = std::make_unique<NativeFn[]>(LS_SIZE / 4);
    for (uint32_t i = 0; i < m->count; i++) native[m->entries[i].pc >> 2] = m->entries[i].fn;
    // code_q for this module: computed once, copied at every map (jobs map hundreds of times a frame).
    {
        static std::mutex lock;
        static std::unordered_map<const NativeModule *, std::vector<uint8_t>> tables;
        thread_local std::unordered_map<const NativeModule *, const uint8_t *> local;
        const uint8_t *&t = local[m];
        uint32_t qn = ((m->ls_base + m->size - 1) >> 4) - (m->ls_base >> 4) + 1;
        if (!t) {
            std::lock_guard<std::mutex> g(lock);
            auto &v = tables[m];
            if (v.empty()) {
                v.resize(qn);
                for (uint32_t q = 0; q < qn; q++) v[q] = is_code(m, q) ? CODE : DATA;
            }
            t = v.data();
        }
        memcpy(code_q + (m->ls_base >> 4), t, qn);
    }
    mapped[slot].m = m;
    if (g_maps++ < 20) LOG("spu", "%s: native code for %s mapped at 0x%x", name.c_str(), m->name, m->ls_base);
    return true;
}

void SpuThread::code_loaded(uint32_t lsa, uint32_t ea, uint32_t size) {
    ls_written(lsa, size);
    loaded_from(lsa, ea, size);
    if (!g_enabled) return;
    auto range = by_source().equal_range(ea);
    for (auto it = range.first; it != range.second; ++it) {
        const NativeModule *m = it->second;
        if (m->ls_base != lsa || m->size > size || !image_matches(ls, m)) continue;
        map_module(m);
        return;
    }
}

// Images that arrive in pieces (DMA lists, several GETs - one DMA moves at most
// 16 KB) never match code_loaded. When execution reaches a pc without native
// code after LS was written, compare LS with every module covering pc.
bool SpuThread::map_at(uint32_t pc) {
    code_dirty = false;
    line_seen[pc >> 7] = 1;
    if (!g_enabled) return false;
    auto try_map = [&](const NativeModule *m) {
        if (pc < m->ls_base || pc >= m->ls_base + m->size || !is_code(m, (pc - m->ls_base) >> 4)) return false;
        for (auto &mp : mapped)
            if (mp.m && m->ls_base < mp.m->ls_base + mp.m->size && mp.m->ls_base < m->ls_base + m->size) {
                static const char *mlog = getenv("KZ2_SPU_MATCH_LOG");
                static std::atomic<int> logs{0};
                static const uint32_t spc = getenv("KZ2_SPU_MATCH_PC") ? (uint32_t)strtoul(getenv("KZ2_SPU_MATCH_PC"), nullptr, 16) : 0;
                if (mlog && logs < 30 && (!strcmp(mlog, "1") || name.find(mlog) != std::string::npos) && (!spc || (pc & ~0x1ffu) == (spc & ~0x1ffu)))
                    logs++, LOG("spu", "%s: %s skipped at pc 0x%x: overlaps mapped %s (0x%x+0x%x)", name.c_str(), m->name, pc,
                                mp.m->name, mp.m->ls_base, mp.m->size);
                return false;
            }
        g_match_tries++;
        if (!image_matches(ls, m)) {
            // Diagnostics (KZ2_SPU_MATCH_LOG=1): which compiled module was rejected, and the
            // first code quadword where LS differs from the image it was compiled from.
            // KZ2_SPU_MATCH_LOG=<text>: only threads whose name contains it ("1": all).
            static const char *mlog = getenv("KZ2_SPU_MATCH_LOG");
            static std::atomic<int> logs{0};
            static const uint32_t mpc = getenv("KZ2_SPU_MATCH_PC") ? (uint32_t)strtoul(getenv("KZ2_SPU_MATCH_PC"), nullptr, 16) : 0;
            if (mlog && logs < 60 && (!strcmp(mlog, "1") || name.find(mlog) != std::string::npos) && (!mpc || (pc & ~0x1ffu) == (mpc & ~0x1ffu))) {
                const uint8_t *img = m->image ? m->image : (const uint8_t *)mem::ptr(m->src_ea);
                for (uint32_t off = 0; off < m->size; off += 16)
                    if (is_code(m, off >> 4) && memcmp(ls + m->ls_base + off, img + off, std::min(16u, m->size - off)) &&
                        (!m->image || !m->protect || (m->protect[off >> 4] & 0xf) == 0xf)) {
                        logs++;
                        const uint32_t *x = (const uint32_t *)(ls + m->ls_base + off), *y = (const uint32_t *)(img + off);
                        LOG("spu", "%s: %s rejected at pc 0x%x: code quadword 0x%x differs (LS %08x %08x %08x %08x, image %08x %08x %08x %08x)",
                            name.c_str(), m->name, pc, m->ls_base + off, x[0], x[1], x[2], x[3], y[0], y[1], y[2], y[3]);
                        break;
                    }
            }
            return false;
        }
        if (!map_module(m)) return false;
        // move to the front of the recently used list
        int k = 0;
        while (k < 7 && recent[k] != m) k++;
        for (; k > 0; k--) recent[k] = recent[k - 1];
        recent[0] = m;
        return true;
    };
    {   // Diagnostics (KZ2_SPU_MATCH_LOG): every map attempt per line, once per line and thread.
        static const char *mlog = getenv("KZ2_SPU_MATCH_LOG");
        static std::atomic<int> logs{0};
        if (mlog && logs < 200 && (!strcmp(mlog, "1") || name.find(mlog) != std::string::npos)) {
            int cover = 0;
            for (const NativeModule *table : {g_native_modules, g_rt_modules}) {
                int n = table == g_native_modules ? g_native_module_count : g_rt_enabled ? g_rt_module_count : 0;
                for (int i = 0; i < n; i++)
                    if (pc >= table[i].ls_base && pc < table[i].ls_base + table[i].size && is_code(&table[i], (pc - table[i].ls_base) >> 4))
                        cover++;
            }
            int used = 0;
            for (auto &mp : mapped) used += mp.m != nullptr;
            logs++;
            LOG("spu", "%s: map_at pc 0x%x: %d modules cover it, %d/8 slots used", name.c_str(), pc, cover, used);
        }
    }
    for (const NativeModule *m : recent)
        if (m && try_map(m)) return native[pc >> 2];
    for (const NativeModule *table : {g_native_modules, g_rt_modules}) {
        int n = table == g_native_modules ? g_native_module_count : g_rt_enabled ? g_rt_module_count : 0;
        for (int i = 0; i < n; i++) {
            if (try_map(&table[i])) return native[pc >> 2];
        }
    }
    return false;
}

void SpuThread::unmap_range(uint32_t lsa, uint32_t size) {
    for (auto &mp : mapped) {
        const NativeModule *m = mp.m;
        if (!m || lsa >= m->ls_base + m->size || m->ls_base >= lsa + size) continue;
        bool code = false;
        for (uint32_t a = std::max(lsa, m->ls_base) & ~15u; a < std::min(lsa + size, m->ls_base + m->size) && !code; a += 16)
            code = is_code(m, (a - m->ls_base) >> 4);
        if (!code) continue;
        for (uint32_t i = 0; i < m->count; i++) native[m->entries[i].pc >> 2] = nullptr;
        memset(code_q + (m->ls_base >> 4), 0, ((m->ls_base + m->size - 1) >> 4) - (m->ls_base >> 4) + 1);
        mp.m = nullptr;
        code_epoch++;
        if (g_unmaps++ < 20) LOG("spu", "%s: native code for %s unmapped (write to 0x%x+0x%x)", name.c_str(), m->name, lsa, size);
    }
}

} // namespace spu
