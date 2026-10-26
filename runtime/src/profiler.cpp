// Sampling profiler (KZ2_PROFILE=<seconds>): about every millisecond every
// other thread is briefly suspended and its instruction pointer recorded; every
// <seconds> the hottest addresses are logged as module+offset (map kz2.exe
// offsets to functions with llvm-nm, image base 0x140000000).
#include "runtime.h"

#include <windows.h>
#include <tlhelp32.h>

#include <algorithm>
#include <cstdlib>
#include <thread>
#include <unordered_map>
#include <vector>

void name_host_thread(const std::string &name) {
    std::wstring w(name.begin(), name.end());
    SetThreadDescription(GetCurrentThread(), w.c_str());
}

namespace {

std::string thread_name(HANDLE h) {
    PWSTR d = nullptr;
    std::string s = "?";
    if (SUCCEEDED(GetThreadDescription(h, &d)) && d) {
        s.clear();
        for (PWSTR p = d; *p; p++) s += (char)*p;
        LocalFree(d);
        if (s.empty()) s = "(unnamed)";
    }
    return s;
}

uint64_t cpu_time(HANDLE h) {
    FILETIME c, e, k, u;
    if (!GetThreadTimes(h, &c, &e, &k, &u)) return 0;
    return ((uint64_t)k.dwHighDateTime << 32 | k.dwLowDateTime) + ((uint64_t)u.dwHighDateTime << 32 | u.dwLowDateTime);
}

std::string where(uint64_t rip) {
    HMODULE mod = nullptr;
    char name[MAX_PATH] = "?";
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)rip, &mod))
        GetModuleFileNameA(mod, name, sizeof name);
    const char *base = strrchr(name, '\\');
    char buf[MAX_PATH + 32];
    snprintf(buf, sizeof buf, "%s+0x%llx", base ? base + 1 : name, (unsigned long long)(rip - (uintptr_t)mod));
    return buf;
}

void profiler(int secs) {
    DWORD self = GetCurrentThreadId(), pid = GetCurrentProcessId();
    std::unordered_map<DWORD, std::unordered_map<uint64_t, uint32_t>> hits;   // per thread: rip bucket -> samples
    std::unordered_map<DWORD, uint64_t> last_cpu, busy;    // per thread, 100 ns units (busy: this period)
    std::unordered_map<DWORD, std::string> names;
    uint64_t samples = 0;
    ULONGLONG next_report = GetTickCount64() + secs * 1000ull;
    for (;;) {
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        THREADENTRY32 te{sizeof te};
        for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
            if (te.th32OwnerProcessID != pid || te.th32ThreadID == self) continue;
            HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_LIMITED_INFORMATION, FALSE,
                                  te.th32ThreadID);
            if (!h) continue;
            // CPU time per thread (accurate over the period; Windows updates it in ~15 ms steps).
            uint64_t t = cpu_time(h), &prev = last_cpu[te.th32ThreadID];
            if (t != prev) busy[te.th32ThreadID] += t > prev ? t - prev : t;   // thread ids get reused
            prev = t;
            if (!names.count(te.th32ThreadID)) names[te.th32ThreadID] = thread_name(h);
            if (SuspendThread(h) != (DWORD)-1) {
                CONTEXT c{};
                c.ContextFlags = CONTEXT_CONTROL;
                if (GetThreadContext(h, &c)) hits[te.th32ThreadID][c.Rip & ~0xfull]++, samples++;   // 16-byte buckets
                ResumeThread(h);
            }
            CloseHandle(h);
        }
        CloseHandle(snap);
        if (GetTickCount64() >= next_report) {
            next_report += secs * 1000ull;
            std::vector<std::pair<uint64_t, DWORD>> threads;
            for (auto &[tid, t] : busy) threads.push_back({t, tid});
            std::sort(threads.rbegin(), threads.rend());
            LOG("profile", "CPU time per thread over %d s (100%% = one core):", secs);
            for (size_t i = 0; i < threads.size() && i < 25; i++)
                LOG("profile", "  %6.1f%%  %s", threads[i].first / 1e5 / secs, names[threads[i].second].c_str());
            // Where the busiest threads spend their time.
            for (size_t i = 0; i < threads.size() && i < 3; i++) {
                auto &th = hits[threads[i].second];
                uint64_t n = 0;
                std::vector<std::pair<uint32_t, uint64_t>> top;
                for (auto &[rip, k] : th) top.push_back({k, rip}), n += k;
                std::sort(top.rbegin(), top.rend());
                LOG("profile", " hot spots of %s (%llu samples):", names[threads[i].second].c_str(), (unsigned long long)n);
                for (size_t j = 0; j < top.size() && j < 20; j++)
                    LOG("profile", "    %5.1f%%  %s", 100.0 * top[j].first / n, where(top[j].second).c_str());
            }
            busy.clear();
            hits.clear(), samples = 0;
        }
        Sleep(1);
    }
}

} // namespace

void start_profiler() {
    if (const char *e = getenv("KZ2_PROFILE")) {
        int secs = atoi(e) > 0 ? atoi(e) : 20;
        std::thread(profiler, secs).detach();
    }
}
