// Mapping recompiled SPU modules (build/generated/spu) into a thread's LS.
// A module maps when its exact image is copied to its LS base - a task ELF
// segment loaded by the runtime, or a job binary DMA'd from the EBOOT - and
// unmaps when anything writes into its range. KZ2_SPU_NATIVE=0 disables it.
#include "spu.h"

#include "runtime.h"

#include <atomic>
#include <cstdlib>
#include <unordered_map>

namespace spu {

extern const NativeModule g_native_modules[];
extern const int g_native_module_count;

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

std::atomic<uint64_t> g_maps{0}, g_unmaps{0};
} // namespace

void SpuThread::code_loaded(uint32_t lsa, uint32_t ea, uint32_t size) {
    ls_written(lsa, size);
    if (!g_enabled) return;
    auto range = by_source().equal_range(ea);
    for (auto it = range.first; it != range.second; ++it) {
        const NativeModule *m = it->second;
        if (m->ls_base != lsa || m->size > size || memcmp(ls + lsa, mem::ptr(ea), m->size)) continue;
        int slot = -1;
        for (int i = 0; i < 8; i++) if (!mapped[i].m) { slot = i; break; }
        if (slot < 0) return;
        if (!native) native = new NativeFn[LS_SIZE / 4]();
        for (uint32_t i = 0; i < m->count; i++) native[m->entries[i].pc >> 2] = m->entries[i].fn;
        for (uint32_t l = m->ls_base >> 7; l <= (m->ls_base + m->size - 1) >> 7; l++) code_line[l] = 1;
        mapped[slot].m = m;
        if (g_maps++ < 20) LOG("spu", "%s: native code for %s mapped at 0x%x", name.c_str(), m->name, lsa);
        return;
    }
}

void SpuThread::unmap_range(uint32_t lsa, uint32_t size) {
    for (auto &mp : mapped) {
        const NativeModule *m = mp.m;
        if (!m || lsa >= m->ls_base + m->size || m->ls_base >= lsa + size) continue;
        for (uint32_t i = 0; i < m->count; i++) native[m->entries[i].pc >> 2] = nullptr;
        for (uint32_t l = m->ls_base >> 7; l <= (m->ls_base + m->size - 1) >> 7; l++) code_line[l] = 0;
        mp.m = nullptr;
        code_epoch++;
        if (g_unmaps++ < 20) LOG("spu", "%s: native code for %s unmapped (write to 0x%x+0x%x)", name.c_str(), m->name, lsa, size);
    }
}

} // namespace spu
