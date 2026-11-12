// Mapping recompiled SPU modules (build/generated/spu) into a thread's LS.
// A module maps when its image is in LS at its base - copied there in one
// piece (code_loaded), or found when execution reaches it (map_at) - and
// unmaps when anything writes to one of its code quadwords. Native code exists
// only for code quadwords, so writes to the rest of the image (data) are harmless.
// EBOOT modules (task ELFs, job binaries) are compared with the EBOOT; runtime
// modules (captured by spu_capture.cpp) with their embedded image, on their
// code quadwords. HELSARI_SPU_NATIVE=0 disables it.
#include "spu.h"

#include "runtime.h"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace rsx { bool pc_depth_of_field(); }   // GRAPHICS menu (pc_options.cpp)

namespace spu {

extern const NativeModule g_native_modules[];      // from the EBOOT (spu_modules.cpp)
extern const int g_native_module_count;
extern const NativeModule g_rt_modules[];          // from spu_cache/ (spu_rt_modules.cpp)
extern const int g_rt_module_count;

namespace {
const bool g_enabled = [] {
    const char *e = getenv("HELSARI_SPU_NATIVE");
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

// HELSARI_SPU_RT=0: ignore modules compiled from spu_cache/ (EBOOT modules still map).
const bool g_rt_enabled = [] {
    const char *e = getenv("HELSARI_SPU_RT");
    return !e || atoi(e) != 0;
}();
} // namespace
std::atomic<uint64_t> g_maps{0}, g_unmaps{0}, g_match_tries{0};   // reported in the flip log (spu_interp.cpp)
namespace {

// A quadword that is all zero in an EBOOT module's image. Zero words decode as STOP, which compiled
// code hands to the interpreter, so whatever LS holds there never runs as compiled code: such
// quadwords are not compared, and writes to them (the program filling in its zeroed data) are data
// writes, not code changes that unmap the module.
bool zero_quad(const NativeModule *m, uint32_t q) {
    const uint8_t *img = (const uint8_t *)mem::ptr(m->src_ea) + q * 16;
    uint64_t a, b;
    memcpy(&a, img, 8), memcpy(&b, img + 8, 8);
    return !(a | b);
}

bool is_code(const NativeModule *m, uint32_t q) {   // q: quadword index from ls_base
    if (m->protect) return m->protect[q];
    return m->image || !zero_quad(m, q);
}

// Does LS hold quadword q (index from ls_base) of m's image? Only the words that were compiled count.
bool quad_matches(const uint8_t *ls, const NativeModule *m, uint32_t q) {
    const uint8_t *img = m->image ? m->image : (const uint8_t *)mem::ptr(m->src_ea);
    const uint32_t off = q * 16, n = std::min(16u, m->size - off);
    const uint8_t bits = m->protect ? (m->protect[q] & 0xf) : 0xf;
    if (bits == 0xf) return !memcmp(ls + m->ls_base + off, img + off, n);
    for (uint32_t w = 0; w < 4 && w * 4 < n; w++)
        if ((bits >> w & 1) && memcmp(ls + m->ls_base + off + w * 4, img + off + w * 4, 4)) return false;
    return true;
}

// Is m's program in LS? Its compiled quadwords, allowing a few that differ: programs keep data among
// their code and change it. Those are mapped STALE (map_module), so they never run as compiled code;
// the threshold only keeps a different program from being mapped. HELSARI_SPU_MATCH_EXACT=1: no slack.
bool program_matches(const uint8_t *ls, const NativeModule *m, std::vector<uint32_t> *differ = nullptr) {
    static const bool exact = getenv("HELSARI_SPU_MATCH_EXACT") != nullptr;
    const uint8_t *img = m->image ? m->image : (const uint8_t *)mem::ptr(m->src_ea);
    if (!m->protect && !memcmp(ls + m->ls_base, img, m->size)) return true;
    uint32_t code = 0, bad = 0;
    for (uint32_t q = 0; q * 16 < m->size; q++) {
        if (!is_code(m, q)) continue;
        code++;
        if (!quad_matches(ls, m, q)) {
            if (exact) return false;
            bad++;
            if (differ) differ->push_back(q);
        }
    }
    return code >= 4 && bad * 16 <= code;
}

} // namespace

// HELSARI_SKIP_JOBS=job_d7aa80,job_d7d100 ("post" = the whole post-processing set): these jobs do
// no pixel work. Measures what a job costs, or (later) lets the GPU do its work. They still run:
// returning at once hangs the game, the job manager relies on the job's calls back into it. So
// the job gets empty job data (no rows, no optional inputs). Job's code starts at its header's
// entry offset, 0x30; r3 = context, context+32 = job data.
// Not in the set: job_d81900, a 4-byte PUT from job data - the signal the frame waits for.
namespace {
const uint32_t JOB_ENTRY = 0x30;
uint32_t skip_job(SpuThread &t, V *g, uint32_t pc) {
    uint32_t data = t.ls32((t.gpr[3].w[0] + 32) & (LS_SIZE - 4));
    t.ls[(data + 2) & (LS_SIZE - 1)] &= ~0x07;   // q0 word 0, bits 4-10: optional inputs
    t.ls[(data + 3) & (LS_SIZE - 1)] &= ~0xf0;
    t.ls[(data + 16 + 5) & (LS_SIZE - 1)] = 0;   // q1 byte 5: rows
    for (auto &mp : t.mapped)
        if (mp.m && mp.m->ls_base + JOB_ENTRY == pc)
            for (uint32_t i = 0; i < mp.m->count; i++)
                if (mp.m->entries[i].pc == pc) return mp.m->entries[i].fn(t, g, pc);
    t.interp_once = true;
    return pc;
}
bool job_skipped(const char *job) {
    static const std::string list = [] {
        const char *e = getenv("HELSARI_SKIP_JOBS");
        std::string s = e ? e : "";
        if (s == "post")
            s = "job_d61f00,job_d69d00,job_d6ce80,job_d7fe00,job_d72e80,job_d66000,"
                "job_d60280,job_d78f00,job_d75780,job_d7aa80,job_d7d100";
        return s;
    }();
    return !list.empty() && strncmp(job, "job_", 4) == 0 && list.find(job) != std::string::npos;
}
}

bool SpuThread::map_module(const NativeModule *m) {
    // Whatever was mapped over this range is gone from LS now (m's image is there).
    for (auto &mp : mapped)
        if (mp.m && mp.m != m && m->ls_base < mp.m->ls_base + mp.m->size && mp.m->ls_base < m->ls_base + m->size) unmap_module(mp);
    int slot = -1;
    for (int i = 0; i < 8; i++) if (!mapped[i].m) { slot = i; break; }
    if (slot < 0) return false;
    if (!native) native = std::make_unique<NativeFn[]>(LS_SIZE / 4);
    for (uint32_t i = 0; i < m->count; i++) native[m->entries[i].pc >> 2] = m->entries[i].fn;
    // job_d7d100 is the depth of field. Jobs map many times a frame, so the menu's choice applies at once.
    if (job_skipped(m->name) || (!::rsx::pc_depth_of_field() && !strcmp(m->name, "job_d7d100")))
        native[(m->ls_base + JOB_ENTRY) >> 2] = skip_job;
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
    // Quadwords LS holds differently (the program's changed data): stale, not run as compiled code.
    {
        std::vector<uint32_t> differ;   // the callers checked that it is m's program
        program_matches(ls, m, &differ);
        for (uint32_t q : differ) code_q[(m->ls_base >> 4) + q] = STALE;
    }
    mapped[slot].m = m;
    if (name[6] == 'j' && !m->image) gcm_post_job_mapped(m->name);   // "spurs:jm_...": F9 buffer dumps
    // The first map of each module on each thread (jobs map hundreds of times a frame).
    g_maps++;
    thread_local std::unordered_set<const NativeModule *> seen;
    if (seen.insert(m).second) LOG("spu", "%s: native code for %s mapped at 0x%x", name.c_str(), m->name, m->ls_base);
    return true;
}

void SpuThread::code_loaded(uint32_t lsa, uint32_t ea, uint32_t size) {
    ls_written(lsa, size);
    loaded_from(lsa, ea, size);
    if (!g_enabled) return;
    auto range = by_source().equal_range(ea);
    for (auto it = range.first; it != range.second; ++it) {
        const NativeModule *m = it->second;
        if (m->ls_base != lsa || m->size > size || !program_matches(ls, m)) continue;
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
            if (mp.m && m->ls_base < mp.m->ls_base + mp.m->size && mp.m->ls_base < m->ls_base + m->size &&
                code_q[pc >> 4] != STALE) {   // overwritten at pc: m may replace it
                static const char *mlog = getenv("HELSARI_SPU_MATCH_LOG");
                static std::atomic<int> logs{0};
                static const uint32_t spc = getenv("HELSARI_SPU_MATCH_PC") ? (uint32_t)strtoul(getenv("HELSARI_SPU_MATCH_PC"), nullptr, 16) : 0;
                if (mlog && logs < 30 && (!strcmp(mlog, "1") || name.find(mlog) != std::string::npos) && (!spc || (pc & ~0x1ffu) == (spc & ~0x1ffu)))
                    logs++, LOG("spu", "%s: %s skipped at pc 0x%x: overlaps mapped %s (0x%x+0x%x)", name.c_str(), m->name, pc,
                                mp.m->name, mp.m->ls_base, mp.m->size);
                return false;
            }
        g_match_tries++;
        if (!program_matches(ls, m)) {
            // Diagnostics (HELSARI_SPU_MATCH_LOG=1): which compiled module was rejected, and the
            // first code quadword where LS differs from the image it was compiled from.
            // HELSARI_SPU_MATCH_LOG=<text>: only threads whose name contains it ("1": all).
            static const char *mlog = getenv("HELSARI_SPU_MATCH_LOG");
            static std::atomic<int> logs{0};
            static const uint32_t mpc = getenv("HELSARI_SPU_MATCH_PC") ? (uint32_t)strtoul(getenv("HELSARI_SPU_MATCH_PC"), nullptr, 16) : 0;
            // HELSARI_SPU_MATCH_MODULE=<text>,<text>: only modules whose name contains one of them.
            static const char *mmod = getenv("HELSARI_SPU_MATCH_MODULE");
            const auto module_wanted = [&] {
                if (!mmod) return true;
                for (const char *p = mmod; *p;) {
                    const char *e = strchr(p, ',');
                    const std::string part(p, e ? e - p : strlen(p));
                    if (!part.empty() && strstr(m->name, part.c_str())) return true;
                    if (!e) break;
                    p = e + 1;
                }
                return false;
            };
            if (mlog && logs < 60 && (!strcmp(mlog, "1") || name.find(mlog) != std::string::npos) && (!mpc || (pc & ~0x1ffu) == (mpc & ~0x1ffu)) &&
                module_wanted()) {
                // How many of its code quadwords differ, and the first few (the match allows 1 in 16).
                uint32_t code = 0;
                std::vector<uint32_t> differ;
                for (uint32_t q = 0; q * 16 < m->size; q++)
                    if (is_code(m, q)) {
                        code++;
                        if (!quad_matches(ls, m, q)) differ.push_back(q);
                    }
                std::string where;
                for (size_t i = 0; i < differ.size() && i < 6; i++) {
                    char b[16];
                    snprintf(b, sizeof b, " 0x%x", m->ls_base + differ[i] * 16);
                    where += b;
                }
                logs++;
                LOG("spu", "%s: %s rejected at pc 0x%x: %zu of %u code quadwords differ, first at%s", name.c_str(), m->name, pc,
                    differ.size(), code, where.c_str());
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
    {   // Diagnostics (HELSARI_SPU_MATCH_LOG): every map attempt per line, once per line and thread.
        static const char *mlog = getenv("HELSARI_SPU_MATCH_LOG");
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
    // Nothing compiled matches, and what is mapped here was overwritten at pc: another program is in
    // LS now. Unmap the old one, so its stale blocks go and the new program can be mapped or captured.
    if (code_q[pc >> 4] == STALE)
        for (auto &mp : mapped)
            if (mp.m && pc >= mp.m->ls_base && pc < mp.m->ls_base + mp.m->size) unmap_module(mp);
    return false;
}

// Before the thread object is reused for another program (the task pool): clear just the native
// entries its mapped modules set, instead of the whole 512 KB table.
void SpuThread::clear_native() {
    if (!native) return;
    for (auto &mp : mapped) {
        if (const NativeModule *m = mp.m) {
            for (uint32_t i = 0; i < m->count; i++) native[m->entries[i].pc >> 2] = nullptr;
            native[(m->ls_base + JOB_ENTRY) >> 2] = nullptr;
            mp.m = nullptr;
        }
    }
}

void SpuThread::unmap_module(Mapped &mp) {
    const NativeModule *m = mp.m;
    for (uint32_t i = 0; i < m->count; i++) native[m->entries[i].pc >> 2] = nullptr;
    if (native[(m->ls_base + JOB_ENTRY) >> 2] == skip_job) native[(m->ls_base + JOB_ENTRY) >> 2] = nullptr;
    memset(code_q + (m->ls_base >> 4), 0, ((m->ls_base + m->size - 1) >> 4) - (m->ls_base >> 4) + 1);
    mp.m = nullptr;
    code_epoch++;
}

uint32_t SpuThread::stale_block(uint32_t pc, uint32_t q) {
    for (auto &mp : mapped) {
        const NativeModule *m = mp.m;
        if (m && q >= (m->ls_base >> 4) && q < ((m->ls_base + m->size + 15) >> 4) && is_code(m, q - (m->ls_base >> 4)) &&
            quad_matches(ls, m, q - (m->ls_base >> 4))) {
            code_q[q] = CODE;
            return pc;
        }
    }
    interp_once = true;   // the interpreter runs it as LS holds it
    return pc;
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
        if (native[(m->ls_base + JOB_ENTRY) >> 2] == skip_job) native[(m->ls_base + JOB_ENTRY) >> 2] = nullptr;
        memset(code_q + (m->ls_base >> 4), 0, ((m->ls_base + m->size - 1) >> 4) - (m->ls_base >> 4) + 1);
        mp.m = nullptr;
        code_epoch++;
        if (g_unmaps++ < 20) LOG("spu", "%s: native code for %s unmapped (write to 0x%x+0x%x)", name.c_str(), m->name, lsa, size);
    }
}

} // namespace spu
