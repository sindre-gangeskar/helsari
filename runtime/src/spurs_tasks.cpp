// SPURS tasksets: the taskset policy module is Sony system code we don't
// have, so this file is its native replacement. The CellSpursTaskset
// structure lives in guest memory with the real layout (task bitsets,
// task_info[]), because SPU-side libspurs code reads and updates it with
// atomics. Each task runs on its own host thread and SpuThread:
//   - ELF PT_LOAD segments go to LS (task code lives at 0x3000..0x40000),
//   - the taskset context is set up at LS 0x2700,
//   - r3 = task argument, r4 = {taskset args, spurs address},
//   - the PM syscall entry at LS 0xA70 is trapped (r3 = syscall, r4 = arg).
// Tasks identified as Edge Zlib keep their native implementation (spurs.cpp).
#include "kernel.h"
#include "spu/spu.h"

#include <atomic>
#include <cstdlib>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#define REG(n) hle_register(#n, hle_##n)

bool spurs_is_native_task(uint32_t taskset, uint32_t elf);   // spurs.cpp

namespace {

constexpr uint32_t TASK_TOP = 0x3000, TASK_BOTTOM = 0x40000;
constexpr uint32_t SYSCALL_ADDR = 0xa70;
constexpr uint32_t TASK_RETURN_ADDR = 0xa80;     // where a task's main() returns to
constexpr uint32_t KERNEL_CTX = 0x100, TASKSET_CTX = 0x2700;
// Inside the PM's stack area (0x2850..0x2C80), unused by tasks.
constexpr uint32_t SERVICE_TRAPS = 0x2900, SERVICE_TABLE = 0x2a00, SERVICE_SPAN = 0x100;
enum { SYS_EXIT = 0, SYS_YIELD = 1, SYS_WAIT_SIGNAL = 2, SYS_POLL = 3, SYS_RECV_WKL_FLAG = 4 };

// CellSpursTaskset offsets
enum { TS_RUNNING = 0x00, TS_READY = 0x10, TS_ENABLED = 0x30, TS_SIGNALLED = 0x40, TS_WAITING = 0x50,
       TS_SPURS = 0x60, TS_ARGS = 0x68, TS_WID = 0x74, TS_TASK_INFO = 0x80, TS_SIZE = 0x1890 };
// TaskInfo (0x30 each): args[16], u64 elf, u64 context, ls_pattern[16]
enum { TI_ARGS = 0x00, TI_ELF = 0x10, TI_CONTEXT = 0x18, TI_LS_PATTERN = 0x20, TI_SIZE = 0x30 };
// Our layout inside the opaque CellSpursTaskAttribute (256 bytes)
enum { TA_ELF = 0x00, TA_CONTEXT = 0x04, TA_CONTEXT_SIZE = 0x08, TA_EXIT_CODE = 0x0c, TA_ARGS = 0x10, TA_LS_PATTERN = 0x20 };

struct Task {
    uint32_t taskset = 0, id = 0, elf = 0, exit_code_addr = 0;
    std::unique_ptr<spu::SpuThread> spu;
    std::atomic<bool> done{false};
};

std::mutex g_lock;
std::map<uint32_t, std::vector<std::shared_ptr<Task>>> g_tasksets;   // taskset ea -> tasks
std::map<uint32_t, bool> g_shutdown;

// Task bitsets are u32[4] big-endian; task i is bit (31 - i % 32) of word i / 32.
uint32_t *bit_word(uint32_t set_ea, uint32_t task) { return (uint32_t *)mem::ptr(set_ea + 4 * (task / 32)); }
uint32_t bit_mask(uint32_t task) { return __builtin_bswap32(0x80000000u >> (task % 32)); }
void bit_set(uint32_t ea, uint32_t t) { __atomic_fetch_or(bit_word(ea, t), bit_mask(t), __ATOMIC_SEQ_CST); }
void bit_clear(uint32_t ea, uint32_t t) { __atomic_fetch_and(bit_word(ea, t), ~bit_mask(t), __ATOMIC_SEQ_CST); }
bool bit_test(uint32_t ea, uint32_t t) { return __atomic_load_n(bit_word(ea, t), __ATOMIC_SEQ_CST) & bit_mask(t); }

// Load an SPU ELF's PT_LOAD segments into LS. Returns the entry point (0 on error).
uint32_t load_elf(spu::SpuThread &s, uint32_t elf) {
    if (ld32(elf) != 0x7f454c46) return 0;
    uint32_t entry = ld32(elf + 24), phoff = ld32(elf + 28);
    uint16_t phentsize = ld16(elf + 42), phnum = ld16(elf + 44);
    for (uint32_t i = 0; i < phnum; i++) {
        uint32_t ph = elf + phoff + i * phentsize;
        if (ld32(ph) != 1) continue;                       // PT_LOAD
        uint32_t off = ld32(ph + 4), vaddr = ld32(ph + 8), filesz = ld32(ph + 16), memsz = ld32(ph + 20);
        if (vaddr < TASK_TOP || vaddr + memsz > TASK_BOTTOM) {
            LOG("spurs", "task ELF 0x%x: segment 0x%x+0x%x outside task area", elf, vaddr, memsz);
            return 0;
        }
        memset(s.ls + vaddr, 0, memsz);
        memcpy(s.ls + vaddr, mem::ptr(elf + off), filesz);
    }
    return entry;
}

void finish(Task &t, uint32_t exit_code) {
    bit_clear(t.taskset + TS_RUNNING, t.id);
    bit_clear(t.taskset + TS_ENABLED, t.id);
    bit_clear(t.taskset + TS_READY, t.id);
    if (t.exit_code_addr) st32(t.exit_code_addr, exit_code);
    t.done = true;
}

void task_thread(std::shared_ptr<Task> tp) {
    Task &t = *tp;
    spu::SpuThread &s = *t.spu;
    uint32_t ts = t.taskset;
    uint32_t info = ts + TS_TASK_INFO + t.id * TI_SIZE;
    char buf[32];
    snprintf(buf, sizeof buf, "task %u (ELF 0x%x)", t.id, t.elf);
    s.name = buf;
    if (const char *only = getenv("KZ2_SPU_TRACE_NAME"))
        if (s.name.find(only) == std::string::npos) s.traced = (int64_t)1 << 40;

    uint32_t entry = load_elf(s, t.elf);
    if (!entry) { LOG("spurs", "%s: cannot load ELF", s.name.c_str()); finish(t, 0); return; }

    // Kernel context (0x100) and taskset context (0x2700) as the PM leaves them.
    uint32_t spurs = ld32(ts + TS_SPURS + 4);
    s.set_ls32(KERNEL_CTX + 0xc4, spurs);
    s.set_ls32(KERNEL_CTX + 0xdc, ld32(ts + TS_WID));
    s.set_ls32(TASKSET_CTX + 0xb8, 0), s.set_ls32(TASKSET_CTX + 0xbc, ts);   // 0x27B8 taskset
    s.set_ls32(TASKSET_CTX + 0xc4, SYSCALL_ADDR);                           // 0x27C4 syscallAddr
    s.set_ls32(TASKSET_CTX + 0xcc, 0);                                      // 0x27CC spuNum
    s.set_ls32(TASKSET_CTX + 0xd0, 31);                                     // 0x27D0 dmaTagId
    s.set_ls32(TASKSET_CTX + 0xd4, t.id);                                   // 0x27D4 taskId
    memcpy(s.ls + 0x2840, "SPURSTASK MODULE", 16);                          // moduleId
    s.set_ls32(0x2fbc, TASK_TOP);                                           // guidAddr
    memcpy(s.ls + 0x2780, mem::ptr(info), TI_SIZE);                          // task info copy

    // Taskset PM service table (tasksetMgmtAddr, 0x2FB8): libspurs calls
    // entries through it (e.g. +0xC4). Each entry points at its own trapped
    // address; calls are logged until the services are implemented.
    for (uint32_t k = 0; k < SERVICE_SPAN; k += 4) {
        s.set_ls32(SERVICE_TABLE + k, SERVICE_TRAPS + k);
        s.set_trap(SERVICE_TRAPS + k);
    }
    s.set_ls32(0x2fb8, SERVICE_TABLE);
    s.set_trap(SYSCALL_ADDR);
    s.set_trap(TASK_RETURN_ADDR);
    s.on_trap = [&t, ts](spu::SpuThread &c) {
        if (c.pc == TASK_RETURN_ADDR) { finish(t, c.gpr[3].w[0]); return false; }
        if (c.pc >= SERVICE_TRAPS && c.pc < SERVICE_TRAPS + SERVICE_SPAN) {
            static std::atomic<uint64_t> seen{0};
            uint32_t off = c.pc - SERVICE_TRAPS;
            if (!(seen.fetch_or(1ull << (off / 4)) & (1ull << (off / 4))))
                LOG("spurs", "%s: taskset service +0x%x called (r3=0x%x r4=0x%x r5=0x%x) - not implemented, returns 0",
                    c.name.c_str(), off, c.gpr[3].w[0], c.gpr[4].w[0], c.gpr[5].w[0]);
            c.gpr[3] = spu::V{{0, 0, 0, 0}};
            c.pc = c.gpr[0].w[0] & (spu::LS_SIZE - 4);
            return true;
        }
        uint32_t num = c.gpr[3].w[0] & 0xf, ret = 0;
        switch (num) {
        case SYS_EXIT:
            finish(t, c.gpr[4].w[0]);
            return false;
        case SYS_WAIT_SIGNAL:
            // Block until the task's signalled bit is set, then consume it.
            bit_set(ts + TS_WAITING, t.id);
            while (!bit_test(ts + TS_SIGNALLED, t.id) && !g_shutdown[ts]) std::this_thread::yield();
            bit_clear(ts + TS_SIGNALLED, t.id);
            bit_clear(ts + TS_WAITING, t.id);
            break;
        case SYS_YIELD: std::this_thread::yield(); break;
        case SYS_POLL: case SYS_RECV_WKL_FLAG: break;
        default:
            LOG("spurs", "%s: unknown task syscall 0x%x", c.name.c_str(), c.gpr[3].w[0]);
            ret = 0x80410903;                                               // CELL_SPURS_TASK_ERROR_NOSYS
            break;
        }
        c.gpr[3] = spu::V{{ret, 0, 0, 0}};
        c.pc = c.gpr[0].w[0] & (spu::LS_SIZE - 4);
        return true;
    };
    s.on_stop = [&t](spu::SpuThread &c, uint32_t code) {
        LOG("spurs", "%s: stop 0x%x at 0x%x", c.name.c_str(), code, c.pc);
        c.dump_history();
        finish(t, 0);
        return false;
    };

    for (auto &g : s.gpr) g = spu::V{};
    s.gpr[0] = spu::V{{TASK_RETURN_ADDR, 0, 0, 0}};
    s.gpr[1] = spu::V{{TASK_BOTTOM - 0x30, 0, 0, 0}};
    for (int i = 0; i < 4; i++) s.gpr[3].w[i] = ld32(info + TI_ARGS + 4 * i);
    s.gpr[4] = spu::V{{ld32(ts + TS_ARGS), ld32(ts + TS_ARGS + 4), 0, spurs}};
    s.pc = entry;
    bit_set(ts + TS_RUNNING, t.id);
    LOG("spurs", "%s: start at 0x%x", s.name.c_str(), entry);
    s.run();
    if (!t.done) finish(t, 0);
    LOG("spurs", "%s: finished", s.name.c_str());
}

// Shared by CreateTask and CreateTaskWithAttribute.
int32_t create_task(uint32_t ts, uint32_t id_out, uint32_t elf, uint32_t context, uint32_t ls_pattern,
                    uint32_t args, uint32_t exit_code_addr) {
    uint32_t id = 0;
    {
        std::lock_guard<std::mutex> lk(g_lock);
        auto &tasks = g_tasksets[ts];
        while (id < 128 && bit_test(ts + TS_ENABLED, id)) id++;
        if (id == 128) return 0x80410902;                                   // CELL_SPURS_TASK_ERROR_AGAIN
        bit_set(ts + TS_ENABLED, id);
        uint32_t info = ts + TS_TASK_INFO + id * TI_SIZE;
        memset(mem::ptr(info), 0, TI_SIZE);
        if (args) memcpy(mem::ptr(info + TI_ARGS), mem::ptr(args), 16);
        st32(info + TI_ELF + 4, elf);
        st32(info + TI_CONTEXT + 4, context);
        if (ls_pattern) memcpy(mem::ptr(info + TI_LS_PATTERN), mem::ptr(ls_pattern), 16);
        auto t = std::make_shared<Task>();
        t->taskset = ts, t->id = id, t->elf = elf, t->exit_code_addr = exit_code_addr;
        tasks.push_back(t);
        if (id_out) st32(id_out, id);
        if (spurs_is_native_task(ts, elf)) {
            // Replaced natively: no SPU thread; it stays "enabled" until the taskset ends.
            return 0;
        }
        t->spu = std::make_unique<spu::SpuThread>();
        bit_set(ts + TS_READY, id);
        std::thread(task_thread, t).detach();
    }
    return 0;
}

} // namespace

HLE(_cellSpursTasksetAttributeInitialize) {
    // (attr, revision, sdkVersion, u64 args, const u8 priority[8], maxContention)
    uint32_t a = ARG32(0);
    memset(mem::ptr(a), 0, 512);
    st32(a + 0x00, ARG32(1));
    st32(a + 0x04, ARG32(2));
    st64(a + 0x08, ARG(3));
    if (ARG32(4)) memcpy(mem::ptr(a + 0x10), mem::ptr(ARG32(4)), 8);
    st32(a + 0x18, ARG32(5));
    st32(a + 0x20, 6400);                                                  // default taskset size
    RET(0);
}
HLE(cellSpursTasksetAttributeSetName) { st32(ARG32(0) + 0x1c, ARG32(1)); RET(0); }
HLE(cellSpursTasksetAttributeSetTasksetSize) { st32(ARG32(0) + 0x20, ARG32(1)); RET(0); }
HLE(cellSpursCreateTasksetWithAttribute) {
    // (spurs, taskset, attr)
    uint32_t spurs = ARG32(0), ts = ARG32(1), a = ARG32(2);
    uint32_t size = ld32(a + 0x20) ? ld32(a + 0x20) : 6400;
    memset(mem::ptr(ts), 0, size);
    st32(ts + TS_SPURS + 4, spurs);
    st64(ts + TS_ARGS, ld64(a + 0x08));
    st32(ts + TS_SIZE, size);
    {
        std::lock_guard<std::mutex> lk(g_lock);
        g_tasksets[ts].clear();
        g_shutdown[ts] = false;
    }
    std::string name = ld32(a + 0x1c) ? mem::cstr(ld32(a + 0x1c)) : "";
    LOG("spurs", "taskset 0x%x '%s' created (args 0x%llx)", ts, name.c_str(), (unsigned long long)ld64(a + 0x08));
    RET(0);
}
HLE(_cellSpursTaskAttributeInitialize) {
    // (attr, revision, sdkVersion, elf, context, sizeContext, lsPattern *, argument *)
    uint32_t a = ARG32(0);
    memset(mem::ptr(a), 0, 256);
    st32(a + TA_ELF, ARG32(3));
    st32(a + TA_CONTEXT, ARG32(4));
    st32(a + TA_CONTEXT_SIZE, ARG32(5));
    if (ARG32(6)) memcpy(mem::ptr(a + TA_LS_PATTERN), mem::ptr(ARG32(6)), 16);
    if (ARG32(7)) memcpy(mem::ptr(a + TA_ARGS), mem::ptr(ARG32(7)), 16);
    RET(0);
}
HLE(cellSpursTaskAttributeSetExitCodeContainer) { st32(ARG32(0) + TA_EXIT_CODE, ARG32(1)); RET(0); }
HLE(cellSpursTaskExitCodeInitialize) { memset(mem::ptr(ARG32(0)), 0, 128); RET(0); }
HLE(cellSpursCreateTaskWithAttribute) {
    // (taskset, u32 *taskId, attr)
    uint32_t a = ARG32(2);
    RET(create_task(ARG32(0), ARG32(1), ld32(a + TA_ELF), ld32(a + TA_CONTEXT), a + TA_LS_PATTERN, a + TA_ARGS,
                    ld32(a + TA_EXIT_CODE)));
}
HLE(cellSpursCreateTask) {
    // (taskset, u32 *taskId, elf, context, sizeContext, lsPattern *, argument *)
    RET(create_task(ARG32(0), ARG32(1), ARG32(2), ARG32(3), ARG32(5), ARG32(6), 0));
}
HLE(cellSpursShutdownTaskset) {
    std::lock_guard<std::mutex> lk(g_lock);
    g_shutdown[ARG32(0)] = true;
    RET(0);
}
HLE(cellSpursJoinTaskset) {
    // Wait for every SPU-run task of the taskset to finish.
    uint32_t ts = ARG32(0);
    for (;;) {
        bool busy = false;
        {
            std::lock_guard<std::mutex> lk(g_lock);
            for (auto &t : g_tasksets[ts]) busy |= t->spu && !t->done;
        }
        if (!busy) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    RET(0);
}
HLE(cellSpursTasksetUnsetExceptionEventHandler) { RET(0); }

void register_spurs_tasks() {
    REG(_cellSpursTasksetAttributeInitialize); REG(cellSpursTasksetAttributeSetName);
    REG(cellSpursTasksetAttributeSetTasksetSize); REG(cellSpursCreateTasksetWithAttribute);
    REG(_cellSpursTaskAttributeInitialize); REG(cellSpursTaskAttributeSetExitCodeContainer);
    REG(cellSpursTaskExitCodeInitialize); REG(cellSpursCreateTaskWithAttribute); REG(cellSpursCreateTask);
    REG(cellSpursShutdownTaskset); REG(cellSpursJoinTaskset); REG(cellSpursTasksetUnsetExceptionEventHandler);
}
