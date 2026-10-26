// Entry point: load the game's data segments from the user's decrypted
// EBOOT, bind HLE imports, start the PS3 main thread.
#include "runtime.h"

#include <windows.h>

#include <filesystem>
#include <thread>
#include <vector>

namespace cfg {
std::string dump_dir = KZ2_DEFAULT_DUMP;
std::string data_dir;
std::string elf_path = KZ2_DEFAULT_ELF;
}

void register_sys();
void register_gcm();
void register_io();
void register_spurs();
void register_spurs_kernel();
void register_spurs_tasks();
void register_sync();
void hle_dump_counts();
void rsx_dump_methods();
void spurs_tasks_dump();

void hle_register_all() {
    register_sys();
    register_gcm();
    register_io();
    register_spurs();
    register_spurs_kernel();
    register_spurs_tasks();
    register_sync();
}

static uint16_t be16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }
static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
static uint64_t be64(const uint8_t *p) { return (uint64_t)be32(p) << 32 | be32(p + 4); }

static void load_elf(const std::string &path) {
    FILE *f = fopen(path.c_str(), "rb");
    if (!f) fatal("cannot open decrypted EBOOT '%s' (run the build, or pass --elf)", path.c_str());
    std::vector<uint8_t> d;
    fseek(f, 0, SEEK_END);
    d.resize(ftell(f));
    fseek(f, 0, SEEK_SET);
    fread(d.data(), 1, d.size(), f);
    fclose(f);
    if (d.size() < 64 || memcmp(d.data(), "\x7f" "ELF", 4)) fatal("'%s' is not a decrypted ELF", path.c_str());
    uint64_t phoff = be64(&d[32]);
    uint16_t phes = be16(&d[54]), phn = be16(&d[56]);
    for (int i = 0; i < phn; i++) {
        const uint8_t *ph = &d[phoff + i * phes];
        uint32_t type = be32(ph);
        uint64_t off = be64(ph + 8), va = be64(ph + 16), fsz = be64(ph + 32), msz = be64(ph + 40);
        if (type != 1 || !msz) continue;
        memcpy(mem::ptr((uint32_t)va), &d[off], fsz);
        memset(mem::ptr((uint32_t)(va + fsz)), 0, msz - fsz);
        LOG("loader", "segment 0x%08llx-0x%08llx (file 0x%llx)", va, va + msz, fsz);
    }
}

static LONG WINAPI on_crash(EXCEPTION_POINTERS *ep) {
    auto *er = ep->ExceptionRecord;
    HMODULE mod = nullptr;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCSTR)er->ExceptionAddress, &mod);
    char name[MAX_PATH] = "?";
    if (mod) GetModuleFileNameA(mod, name, MAX_PATH);
    LOG("CRASH", "exception 0x%08lx at %p (%s+0x%llx), access address 0x%llx", er->ExceptionCode, er->ExceptionAddress,
        strrchr(name, 92) ? strrchr(name, 92) + 1 : name, (unsigned long long)((uint8_t *)er->ExceptionAddress - (uint8_t *)mod),
        er->NumberParameters > 1 ? (unsigned long long)er->ExceptionInformation[1] : 0ull);
    fflush(stdout);
    return EXCEPTION_EXECUTE_HANDLER;
}

int main(int argc, char **argv) {
    SetUnhandledExceptionFilter(on_crash);
    SetProcessDPIAware();  // render at real pixel size on scaled displays
    std::filesystem::path exe_dir = std::filesystem::path(argv[0]).parent_path();
    cfg::data_dir = (exe_dir / "data").string();
    for (int i = 1; i + 1 < argc; i++) {
        std::string a = argv[i];
        if (a == "--dump") cfg::dump_dir = argv[++i];
        else if (a == "--elf") cfg::elf_path = argv[++i];
        else if (a == "--data") cfg::data_dir = argv[++i];
    }
    for (auto *s : {&cfg::dump_dir, &cfg::data_dir})
        for (auto &c : *s) if (c == '\\') c = '/';
    if (!std::filesystem::exists(cfg::dump_dir + "/PS3_GAME/PARAM.SFO"))
        fatal("Killzone 2 dump not found at '%s' (pass --dump <folder containing PS3_GAME>)", cfg::dump_dir.c_str());
    for (const char *d : {"/dev_hdd0/game", "/dev_hdd0/home/00000001/savedata", "/dev_hdd1"})
        std::filesystem::create_directories(cfg::data_dir + d);
    LOG("main", "dump: %s", cfg::dump_dir.c_str());
    LOG("main", "data: %s", cfg::data_dir.c_str());

    mem::init();
    load_elf(cfg::elf_path);
    hle_register_all();
    hle_install_imports();

    // argv/envp for crt0: 64-bit pointer arrays
    uint32_t path = mem::alloc(64);
    mem::write_cstr(path, "/dev_bdvd/PS3_GAME/USRDIR/EBOOT.BIN", 64);
    uint32_t av = mem::alloc(16), ev = mem::alloc(16);
    st64(av, path);
    st64(av + 8, 0);
    st64(ev, 0);
    if (const char *w = getenv("KZ2_WATCHDOG")) {
        int secs = atoi(w) > 0 ? atoi(w) : 10;
        std::thread([secs] {
            for (;;) {
                Sleep(secs * 1000);
                LOG("watchdog", "thread states:");
                thread::dump_all();
                LOG("watchdog", "most-called imports:");
                hle_dump_counts();
                rsx_dump_methods();
                spurs_tasks_dump();
            }
        }).detach();
    }
    thread::run_main(av, ev);
    LOG("main", "main thread returned");
    return 0;
}
