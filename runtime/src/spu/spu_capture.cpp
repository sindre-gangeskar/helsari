// Runtime capture of SPU code the recompiler has not seen.
//
// SPU programs that are not in the EBOOT (job binaries loaded from the game's
// data files) run in the interpreter. When that happens, the image they were
// loaded as - the LS lines filled by the same DMA load (equal ea - lsa) - is
// snapshotted on first execution, and saved to spu_cache/<ls>_<hash>.bin with
// a mask of the quadwords the interpreter executed. The build
// (tools/recomp/spu_cache.py) compiles those; the next run maps them
// like any other recompiled module. Masks of the same image merge across runs.
// KZ2_SPU_CAPTURE=0 disables it.
#include "spu.h"

#include "runtime.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <unordered_set>
#include <mutex>
#include <unordered_map>

namespace spu {
namespace {

const bool g_capture_on = [] {
    const char *e = getenv("KZ2_SPU_CAPTURE");
    return !e || atoi(e) != 0;
}();
constexpr uint32_t MAX_IMAGE = 0x30000;
constexpr const char *CACHE_DIR = "spu_cache";

// One captured program. Captures of the same program differ in their data, so
// a capture joins an entry with the same LS base and size that agrees on every
// quadword either executed (merging masks and the newly executed bytes); only
// different code starts a new entry. tools/recomp/spu_cache_compact.py applies
// the same rule to an existing cache.
struct Entry {
    uint32_t ls_base;
    std::vector<uint8_t> image, mask;
    std::string path;              // its file, named after the image it was first saved with
};
std::mutex g_lock;
std::vector<Entry> g_cache;
bool g_loaded = false;

uint64_t image_hash(uint32_t ls_base, const uint8_t *p, size_t n) {
    uint64_t h = 1469598103934665603ull ^ ls_base;
    for (size_t i = 0; i < n; i++) h = (h ^ p[i]) * 1099511628211ull;
    return h;
}

// Mask bytes: 0x10 | one bit per executed word of the quadword. Older files hold 1 for
// "executed" (the whole quadword). Comparing per word matters: programs keep data
// words next to their code (a variable after a branch) that differ between loads.
uint32_t word_bits(uint8_t v) { return !v ? 0 : (v & 0x10) ? (v & 0xf) : 0xf; }
bool same_words(const uint8_t *a, const uint8_t *b, uint32_t bits, size_t avail) {
    for (uint32_t w = 0; w < 4 && w * 4 < avail; w++)
        if ((bits >> w & 1) && memcmp(a + w * 4, b + w * 4, 4)) return false;
    return true;
}
bool same_code(const Entry &e, const std::vector<uint8_t> &image, const std::vector<uint8_t> &mask) {
    for (size_t q = 0; q < mask.size(); q++)
        if (uint32_t bits = word_bits(mask[q]) | word_bits(e.mask[q]))
            if (!same_words(&e.image[q * 16], &image[q * 16], bits, image.size() - q * 16)) return false;
    return true;
}

// File: "SPC2", ls_base, size, mask (one byte per quadword), image.
// "SPC1" files (one mask byte per 128-byte line) are read as all 8 quadwords.
constexpr uint32_t SPC1 = 0x31435053, SPC2 = 0x32435053;
void load_cache() {
    g_loaded = true;
    std::error_code ec;
    for (auto &f : std::filesystem::directory_iterator(CACHE_DIR, ec)) {
        if (f.path().extension() != ".bin") continue;
        FILE *fp = _wfopen(f.path().c_str(), L"rb");
        if (!fp) continue;
        uint32_t hdr[3];
        if (fread(hdr, 4, 3, fp) == 3 && (hdr[0] == SPC1 || hdr[0] == SPC2) && hdr[2] <= MAX_IMAGE) {
            uint32_t grain = hdr[0] == SPC1 ? 128 : 16;
            std::vector<uint8_t> file_mask((hdr[2] + grain - 1) / grain), image(hdr[2]);
            if (fread(file_mask.data(), 1, file_mask.size(), fp) == file_mask.size() &&
                fread(image.data(), 1, image.size(), fp) == image.size()) {
                std::vector<uint8_t> mask((hdr[2] + 15) / 16);
                for (size_t q = 0; q < mask.size(); q++) mask[q] = file_mask[q * 16 / grain];
                g_cache.push_back({hdr[1], std::move(image), std::move(mask), f.path().string()});
            }
        }
        fclose(fp);
    }
    if (!g_cache.empty()) LOG("spu", "capture: %zu cached SPU programs", g_cache.size());
}

void save(uint32_t ls_base, const std::vector<uint8_t> &image, const std::vector<uint8_t> &mask) {
    std::lock_guard<std::mutex> g(g_lock);
    if (!g_loaded) load_cache();
    Entry *e = nullptr;
    for (auto &c : g_cache)
        if (c.ls_base == ls_base && c.image.size() == image.size() && same_code(c, image, mask)) { e = &c; break; }
    bool grew = !e;
    if (!e) {
        char path[64];
        snprintf(path, sizeof path, "%s/%05x_%016llx.bin", CACHE_DIR, ls_base,
                 (unsigned long long)image_hash(ls_base, image.data(), image.size()));
        g_cache.push_back({ls_base, image, std::vector<uint8_t>(mask.size()), path});
        e = &g_cache.back();
    }
    for (size_t q = 0; q < mask.size(); q++) {
        uint32_t had = word_bits(e->mask[q]), add = word_bits(mask[q]) & ~had;
        if (!add) continue;
        e->mask[q] = (uint8_t)(0x10 | had | add), grew = true;
        for (uint32_t w = 0; w < 4 && q * 16 + w * 4 < image.size(); w++)
            if (add >> w & 1) memcpy(&e->image[q * 16 + w * 4], &image[q * 16 + w * 4], 4);
    }
    if (!grew) return;
    std::filesystem::create_directories(CACHE_DIR);
    // Written to a temporary name and renamed: a killed process never leaves a partial file.
    std::string tmp = e->path + ".tmp";
    if (FILE *fp = fopen(tmp.c_str(), "wb")) {
        uint32_t hdr[3] = {SPC2, ls_base, (uint32_t)e->image.size()};
        fwrite(hdr, 4, 3, fp);
        fwrite(e->mask.data(), 1, e->mask.size(), fp);
        fwrite(e->image.data(), 1, e->image.size(), fp);
        fclose(fp);
        std::error_code ec;
        std::filesystem::rename(tmp, e->path, ec);
    }
    static int logs;
    if (logs++ < 30) {
        size_t qws = 0;
        for (uint8_t b : e->mask) qws += __builtin_popcount(word_bits(b));
        LOG("spu", "capture: %s (%u bytes at LS 0x%x, %zu instructions)", e->path.c_str(), (unsigned)image.size(), ls_base,
            qws);
    }
}
} // namespace

// spu_cache/entries.txt: "<module> <pc>" for code of a mapped module that was entered
// without a compiled entry point. The build adds them (tools/recomp/spu.py), which turns
// routines reached only through computed jumps from interpreted into compiled code.
void SpuThread::record_entry(uint32_t pc) {
    if (!g_capture_on) return;
    const NativeModule *m = nullptr;
    for (auto &mp : mapped)
        if (mp.m && pc >= mp.m->ls_base && pc < mp.m->ls_base + mp.m->size) m = mp.m;
    if (!m) return;
    static thread_local std::unordered_set<uint64_t> noted;   // no lock on repeats
    if (!noted.insert((uint64_t)(uintptr_t)m << 18 ^ pc).second) return;
    std::lock_guard<std::mutex> g(g_lock);
    static std::set<std::string> saved = [] {
        std::set<std::string> s;
        std::ifstream f(std::string(CACHE_DIR) + "/entries.txt");
        for (std::string line; std::getline(f, line);) s.insert(line);
        return s;
    }();
    char key[96];
    snprintf(key, sizeof key, "%s %x", m->name, pc);
    if (!saved.insert(key).second) return;
    std::filesystem::create_directories(CACHE_DIR);
    if (FILE *fp = fopen((std::string(CACHE_DIR) + "/entries.txt").c_str(), "a")) {
        fprintf(fp, "%s\n", key);
        fclose(fp);
    }
    static int logs;
    if (logs++ < 20) LOG("spu", "%s: new entry point for the next build: %s", name.c_str(), key);
}

// Images are found per 128-byte line (src_delta); a line overlapping mapped native code is not captured.
static bool line_mapped(const SpuThread &t, uint32_t line) {
    for (uint32_t q = line * 8; q < line * 8 + 8; q++)
        if (t.code_q[q]) return true;
    return false;
}

void SpuThread::begin_capture(uint32_t pc) {
    // Reuse the least recently used slot.
    Capture *slot = &caps[0];
    for (auto &c : caps) if (c.last_use < slot->last_use) slot = &c;
    flush_capture(*slot);
    cap_any = true;
    uint32_t l = pc >> 7, d = src_delta[l];
    slot->last_use = ++cap_clock;
    slot->lo = l << 7, slot->hi = slot->lo + 128, slot->valid = false;   // nothing to capture: remember the line
    // Inside a mapped captured module: code that did not run when it was captured (often in
    // the menu; a new path in-game). Re-capture over the module's own range: the save merges
    // the newly executed words into its file, and the next build compiles them.
    if (g_capture_on)
        for (auto &mp : mapped)
            if (mp.m && mp.m->image && pc >= mp.m->ls_base && pc < mp.m->ls_base + mp.m->size &&
                !(mp.m->ls_base & 127) && !(mp.m->size & 127) && mp.m->size <= MAX_IMAGE) {
                slot->lo = mp.m->ls_base, slot->hi = mp.m->ls_base + mp.m->size, slot->valid = true;
                slot->image.assign(ls + slot->lo, ls + slot->hi);
                memset(exec_q + (slot->lo >> 4), 0, mp.m->size >> 4);
                exec_q[pc >> 4] = 0x10 | (1 << ((pc >> 2) & 3));
                return;
            }
    if (!g_capture_on || !d || line_mapped(*this, l)) {
        static std::atomic<int> logs{0};
        if (g_capture_on && pc >= 0x5000 && pc < 0x3f000 && logs++ < 30) LOG("spu", "%s: no capture at pc 0x%x (delta 0x%x, mapped %d)", name.c_str(), pc, d, (int)line_mapped(*this, l));
        return;
    }
    uint32_t lo = l, hi = l + 1;
    while (lo > 0 && src_delta[lo - 1] == d && !line_mapped(*this, lo - 1)) lo--;
    while (hi < LS_SIZE / 128 && src_delta[hi] == d && !line_mapped(*this, hi)) hi++;
    if ((hi - lo) * 128 > MAX_IMAGE) {
        static std::atomic<int> logs{0};
        if (logs++ < 20) LOG("spu", "%s: no capture at pc 0x%x: image 0x%x-0x%x too large", name.c_str(), pc, lo << 7, hi << 7);
        return;
    }
    slot->lo = lo << 7, slot->hi = hi << 7, slot->valid = true;
    slot->image.assign(ls + slot->lo, ls + slot->hi);
    memset(exec_q + lo * 8, 0, (hi - lo) * 8);
    exec_q[pc >> 4] = 0x10 | (1 << ((pc >> 2) & 3));
}

void SpuThread::flush_capture(Capture &c) {
    cap_hot_size = 0;   // note_interpreted's remembered slot may be this one
    if (c.valid) {
        std::vector<uint8_t> mask(exec_q + (c.lo >> 4), exec_q + (c.hi >> 4));
        for (uint8_t b : mask)
            if (b) { save(c.lo, c.image, mask); break; }
    }
    c = Capture{};
    cap_any = false;
    for (auto &o : caps) cap_any |= o.hi != 0;
}

// A DMA load is writing [lsa, lsa+size): if it replaces executed code of a
// capture (a new program), save that capture first. Loads into its data
// (a job's input buffers) leave it alone - the snapshot is already taken.
void SpuThread::capture_overwritten(uint32_t lsa, uint32_t size) {
    for (auto &c : caps) {
        if (!c.hi || lsa >= c.hi || c.lo >= lsa + size) continue;
        uint32_t a = std::max(lsa, c.lo) >> 4, b = (std::min(lsa + size, c.hi) - 1) >> 4;
        bool code = !c.valid;
        for (uint32_t q = a; q <= b && !code; q++) code = exec_q[q];
        if (code) flush_capture(c);
    }
}

// Periodically, images still in LS are saved too (long-lived programs are never replaced).
void SpuThread::checkpoint_captures() {
    for (auto &c : caps) {
        if (!c.valid) continue;
        std::vector<uint8_t> mask(exec_q + (c.lo >> 4), exec_q + (c.hi >> 4));
        for (uint8_t b : mask)
            if (b) { save(c.lo, c.image, mask); break; }
    }
}

} // namespace spu
