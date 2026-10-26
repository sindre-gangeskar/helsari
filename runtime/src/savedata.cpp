// cellSaveData: the auto load/save flow with the game's callbacks.
//
//   funcStat(result, statGet, statSet): we describe the save (new or not,
//       its PARAM.SFO fields, its files); the game says whether to go on and,
//       when saving, supplies the PARAM.SFO fields to write.
//   funcFile(result, fileGet, fileSet): repeated; each call asks for one file
//       operation (read / write / delete) until the game answers OK_LAST.
//
// Saves live in <data>/dev_hdd0/home/00000001/savedata/<dir>/ with a real
// PSF PARAM.SFO. Secure files are stored unencrypted.
#include "kernel.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#define REG(n) hle_register(#n, hle_##n)

namespace stdfs = std::filesystem;

namespace {

constexpr int32_t ERROR_CBRESULT = (int32_t)0x8002b401, ERROR_ACCESS = (int32_t)0x8002b402,
                  ERROR_PARAM = (int32_t)0x8002b404, ERROR_NODATA = (int32_t)0x8002b40b;
enum { CB_OK_NEXT = 0, CB_OK_LAST = 1, CB_OK_LAST_NOCONFIRM = 2 };
enum { FILEOP_READ = 0, FILEOP_WRITE = 1, FILEOP_DELETE = 2, FILEOP_WRITE_NOTRUNC = 3 };

// Guest structure layouts (big endian, 32-bit pointers).
enum { RES_RESULT = 0, RES_SIZE = 56 };
enum { PARAM_TITLE = 0, PARAM_SUBTITLE = 128, PARAM_DETAIL = 256, PARAM_ATTRIBUTE = 1280, PARAM_LISTPARAM = 1288,
       PARAM_SIZE = 1552 };
enum { GET_FREE_KB = 0, GET_IS_NEW = 4, GET_DIR = 8, GET_PARAM = 64, GET_BIND = 1616, GET_SIZE_KB = 1620,
       GET_SYS_SIZE_KB = 1624, GET_FILE_NUM = 1628, GET_FILE_LIST_NUM = 1632, GET_FILE_LIST = 1636, GET_SIZE = 1704 };
enum { SET_PARAM = 0, SET_RECREATE = 4, SET_INDICATOR = 8, SET_SIZE = 16 };
enum { FSTAT_TYPE = 0, FSTAT_SIZE = 8, FSTAT_ATIME = 16, FSTAT_MTIME = 24, FSTAT_CTIME = 32, FSTAT_NAME = 40,
       FSTAT_BYTES = 56 };
enum { FGET_EXC_SIZE = 0, FGET_SIZE = 68 };
enum { FSET_OP = 0, FSET_TYPE = 8, FSET_NAME = 28, FSET_OFFSET = 32, FSET_SIZE = 36, FSET_BUF_SIZE = 40, FSET_BUF = 44,
       FSET_BYTES = 48 };
enum { BUF_FILE_LIST_MAX = 4, BUF_SIZE = 32, BUF_BUF = 36 };

const char *content_name(uint32_t type) {
    switch (type) {
    case 2: return "ICON0.PNG";
    case 3: return "ICON1.PAM";
    case 4: return "PIC1.PNG";
    case 5: return "SND0.AT3";
    default: return nullptr;
    }
}

/* ---- PARAM.SFO (PSF, little endian) ---- */
struct SfoValue { uint16_t fmt; std::vector<uint8_t> data; uint32_t max; };
using Sfo = std::map<std::string, SfoValue>;

Sfo read_sfo(const stdfs::path &p) {
    Sfo sfo;
    std::ifstream f(p, std::ios::binary);
    std::vector<uint8_t> d((std::istreambuf_iterator<char>(f)), {});
    auto u32 = [&](size_t o) { return o + 4 <= d.size() ? d[o] | d[o + 1] << 8 | d[o + 2] << 16 | (uint32_t)d[o + 3] << 24 : 0u; };
    if (d.size() < 20 || u32(0) != 0x46535000) return sfo;
    uint32_t keys = u32(8), data = u32(12), n = u32(16);
    for (uint32_t i = 0; i < n && 20 + 16 * i + 16 <= d.size(); i++) {
        size_t e = 20 + 16 * i;
        uint32_t key = keys + (d[e] | d[e + 1] << 8), fmt = d[e + 2] | d[e + 3] << 8;
        uint32_t len = u32(e + 4), max = u32(e + 8), off = data + u32(e + 12);
        if (key >= d.size() || off + len > d.size()) continue;
        std::string name((const char *)&d[key]);
        sfo[name] = {(uint16_t)fmt, std::vector<uint8_t>(d.begin() + off, d.begin() + off + len), max};
    }
    return sfo;
}

void write_sfo(const stdfs::path &p, const Sfo &sfo) {
    std::vector<uint8_t> keys, data, index;
    auto put32 = [](std::vector<uint8_t> &v, uint32_t x) { for (int k = 0; k < 4; k++) v.push_back((uint8_t)(x >> 8 * k)); };
    for (auto &[name, v] : sfo) {                                  // std::map keeps keys sorted, as PSF requires
        index.push_back((uint8_t)keys.size()), index.push_back((uint8_t)(keys.size() >> 8));
        index.push_back((uint8_t)v.fmt), index.push_back((uint8_t)(v.fmt >> 8));
        put32(index, (uint32_t)v.data.size());
        put32(index, v.max);
        put32(index, (uint32_t)data.size());
        keys.insert(keys.end(), name.begin(), name.end());
        keys.push_back(0);
        data.insert(data.end(), v.data.begin(), v.data.end());
        data.resize(data.size() + (v.max - v.data.size()));
    }
    while (keys.size() % 4) keys.push_back(0);
    std::vector<uint8_t> out;
    put32(out, 0x46535000), put32(out, 0x101);
    put32(out, 20 + (uint32_t)index.size()), put32(out, 20 + (uint32_t)(index.size() + keys.size()));
    put32(out, (uint32_t)sfo.size());
    out.insert(out.end(), index.begin(), index.end());
    out.insert(out.end(), keys.begin(), keys.end());
    out.insert(out.end(), data.begin(), data.end());
    std::ofstream(p, std::ios::binary).write((const char *)out.data(), (std::streamsize)out.size());
}

SfoValue sfo_str(const std::string &s, uint32_t max) {
    std::vector<uint8_t> v(s.begin(), s.end());
    v.push_back(0);
    if (v.size() > max) v.resize(max), v.back() = 0;
    return {0x0204, v, max};
}
SfoValue sfo_int(uint32_t x) { return {0x0404, {(uint8_t)x, (uint8_t)(x >> 8), (uint8_t)(x >> 16), (uint8_t)(x >> 24)}, 4}; }
SfoValue sfo_bin(uint32_t len) { return {0x0004, std::vector<uint8_t>(len), len}; }
std::string sfo_get(const Sfo &sfo, const char *key) {
    auto it = sfo.find(key);
    return it == sfo.end() ? std::string() : std::string((const char *)it->second.data.data(), strnlen((const char *)it->second.data.data(), it->second.data.size()));
}

stdfs::path save_dir(const std::string &dir) { return stdfs::path(cfg::data_dir) / "dev_hdd0/home/00000001/savedata" / dir; }

uint64_t mtime_secs(const stdfs::path &p) {
    std::error_code ec;
    auto t = stdfs::last_write_time(p, ec);
    if (ec) return 0;
    auto sys = std::chrono::system_clock::now() + (t - stdfs::file_time_type::clock::now());   // file clock -> wall clock
    return (uint64_t)std::chrono::duration_cast<std::chrono::seconds>(sys.time_since_epoch()).count();
}

// The flow shared by auto load and auto save.
int32_t auto_op(PPUContext *ctx, bool save) {
    // (version, userId, dirName, errDialog, setBuf, funcStat, funcFile, container, userdata on the stack)
    std::string dir = mem::cstr(ARG32(2));
    uint32_t set_buf = ARG32(4), func_stat = ARG32(5), func_file = ARG32(6), userdata = ld32((uint32_t)ctx->r[1] + 0x74);
    if (dir.empty() || !func_stat || !func_file || !set_buf) return ERROR_PARAM;
    stdfs::path path = save_dir(dir);
    std::error_code ec;
    bool is_new = !stdfs::is_regular_file(path / "PARAM.SFO", ec);
    LOG("savedata", "auto %s '%s' (%s)", save ? "save" : "load", dir.c_str(), is_new ? "new" : "exists");
    if (!save && is_new) return ERROR_NODATA;

    uint32_t res = mem::alloc(RES_SIZE), get = mem::alloc(GET_SIZE), set = mem::alloc(SET_SIZE);
    uint32_t fget = mem::alloc(FGET_SIZE), fset = mem::alloc(FSET_BYTES);
    memset(mem::ptr(res), 0, RES_SIZE), memset(mem::ptr(get), 0, GET_SIZE), memset(mem::ptr(set), 0, SET_SIZE);
    memset(mem::ptr(fget), 0, FGET_SIZE);
    st32(res + 16, userdata);                                       // result->userdata
    auto cleanup = [&](int32_t r) { mem::free(res), mem::free(get), mem::free(set), mem::free(fget), mem::free(fset); return r; };

    // Status
    Sfo sfo = read_sfo(path / "PARAM.SFO");
    st32(get + GET_FREE_KB, 10 * 1024 * 1024);
    st32(get + GET_IS_NEW, is_new);
    uint64_t now = mtime_secs(path / "PARAM.SFO");
    for (int k = 0; k < 3; k++) st64(get + GET_DIR + 8 * k, now);
    mem::write_cstr(get + GET_DIR + 24, dir, 32);
    mem::write_cstr(get + GET_PARAM + PARAM_TITLE, sfo_get(sfo, "TITLE"), 128);
    mem::write_cstr(get + GET_PARAM + PARAM_SUBTITLE, sfo_get(sfo, "SUB_TITLE"), 128);
    mem::write_cstr(get + GET_PARAM + PARAM_DETAIL, sfo_get(sfo, "DETAIL"), 1024);
    if (auto it = sfo.find("ATTRIBUTE"); it != sfo.end() && it->second.data.size() == 4)
        st32(get + GET_PARAM + PARAM_ATTRIBUTE, it->second.data[0] | it->second.data[1] << 8 | it->second.data[2] << 16 | it->second.data[3] << 24);
    mem::write_cstr(get + GET_PARAM + PARAM_LISTPARAM, sfo_get(sfo, "SAVEDATA_LIST_PARAM"), 8);
    uint32_t files = 0, list_max = ld32(set_buf + BUF_FILE_LIST_MAX), list = ld32(set_buf + BUF_BUF);
    uint64_t total = 0;
    if (!is_new)
        for (auto &e : stdfs::directory_iterator(path, ec)) {
            if (!e.is_regular_file() || e.path().filename() == "PARAM.SFO") continue;
            uint64_t size = e.file_size(ec);
            total += size;
            if (files < list_max && list) {
                uint32_t fs = list + files * FSTAT_BYTES;
                memset(mem::ptr(fs), 0, FSTAT_BYTES);
                std::string name = e.path().filename().string();
                uint32_t type = 1;                                  // normal file unless a content file
                for (uint32_t t = 2; t <= 5; t++) if (name == content_name(t)) type = t;
                st32(fs + FSTAT_TYPE, type);
                st64(fs + FSTAT_SIZE, size);
                for (int k = 0; k < 3; k++) st64(fs + FSTAT_ATIME + 8 * k, mtime_secs(e.path()));
                mem::write_cstr(fs + FSTAT_NAME, name, 13);
            }
            files++;
        }
    st32(get + GET_SIZE_KB, (uint32_t)((total + 1023) / 1024) + 35);
    st32(get + GET_SYS_SIZE_KB, 35);
    st32(get + GET_FILE_NUM, files);
    st32(get + GET_FILE_LIST_NUM, std::min(files, list_max));
    st32(get + GET_FILE_LIST, list);

    ctx->r[3] = res, ctx->r[4] = get, ctx->r[5] = set;
    ppu_invoke(ctx, func_stat);
    int32_t r = (int32_t)ld32(res + RES_RESULT);
    LOG("savedata", "  funcStat -> %d (setParam 0x%x reCreate 0x%x indicator 0x%x errNeedSizeKB %d)", r, ld32(set + SET_PARAM),
        ld32(set + SET_RECREATE), ld32(set + SET_INDICATOR), (int32_t)ld32(res + 8));
    if (r < 0) return cleanup(ERROR_CBRESULT);

    if (save) {
        stdfs::create_directories(path, ec);
        if (ec) return cleanup(ERROR_ACCESS);
        if ((ld32(set + SET_RECREATE) & 0xff) >= 2)                 // CELL_SAVEDATA_RECREATE_YES(_RESET_OWNER)
            for (auto &e : stdfs::directory_iterator(path, ec)) stdfs::remove(e.path(), ec);
        if (uint32_t p = ld32(set + SET_PARAM)) {
            Sfo out = sfo;
            out["ACCOUNT_ID"] = sfo_bin(16);
            out["ATTRIBUTE"] = sfo_int(ld32(p + PARAM_ATTRIBUTE));
            out["CATEGORY"] = sfo_str("SD", 4);
            out["DETAIL"] = sfo_str(mem::cstr(p + PARAM_DETAIL), 1024);
            if (!out.count("PARAMS")) out["PARAMS"] = sfo_bin(1024);
            if (!out.count("PARAMS2")) out["PARAMS2"] = sfo_bin(12);
            out["PARENTAL_LEVEL"] = sfo_int(0);
            out["SAVEDATA_DIRECTORY"] = sfo_str(dir, 64);
            out["SAVEDATA_LIST_PARAM"] = sfo_str(std::string((const char *)mem::ptr(p + PARAM_LISTPARAM), strnlen((const char *)mem::ptr(p + PARAM_LISTPARAM), 8)), 8);
            out["SUB_TITLE"] = sfo_str(mem::cstr(p + PARAM_SUBTITLE), 128);
            out["TITLE"] = sfo_str(mem::cstr(p + PARAM_TITLE), 128);
            write_sfo(path / "PARAM.SFO", out);
        }
    }
    if (r == CB_OK_LAST || r == CB_OK_LAST_NOCONFIRM) return cleanup(0);

    // Files
    for (int step = 0; step < 4096; step++) {
        memset(mem::ptr(fset), 0, FSET_BYTES);
        ctx->r[3] = res, ctx->r[4] = fget, ctx->r[5] = fset;
        ppu_invoke(ctx, func_file);
        r = (int32_t)ld32(res + RES_RESULT);
        if (r < 0) { LOG("savedata", "  funcFile -> %d", r); return cleanup(ERROR_CBRESULT); }
        if (r == CB_OK_LAST || r == CB_OK_LAST_NOCONFIRM) break;
        uint32_t op = ld32(fset + FSET_OP), type = ld32(fset + FSET_TYPE);
        std::string name = content_name(type) ? content_name(type) : mem::cstr(ld32(fset + FSET_NAME));
        uint32_t off = ld32(fset + FSET_OFFSET), size = ld32(fset + FSET_SIZE), buf = ld32(fset + FSET_BUF);
        uint32_t buf_size = ld32(fset + FSET_BUF_SIZE), done = 0;
        stdfs::path file = path / name;
        if (op == FILEOP_READ) {
            std::ifstream f(file, std::ios::binary);
            if (f) {
                f.seekg(off);
                mem::ensure(buf, std::min(size, buf_size));
                f.read((char *)mem::ptr(buf), std::min(size, buf_size));
                done = (uint32_t)f.gcount();
            }
        } else if (op == FILEOP_WRITE || op == FILEOP_WRITE_NOTRUNC) {
            stdfs::create_directories(path, ec);
            if (op == FILEOP_WRITE || !stdfs::exists(file, ec)) std::ofstream(file, std::ios::binary);   // create / truncate
            std::fstream f(file, std::ios::binary | std::ios::in | std::ios::out);
            f.seekp(off);
            f.write((const char *)mem::ptr(buf), std::min(size, buf_size));
            done = f ? std::min(size, buf_size) : 0;
        } else if (op == FILEOP_DELETE) {
            stdfs::remove(file, ec);
        }
        LOG("savedata", "  file %s '%s' offset %u size %u (buffer 0x%x, %u bytes) -> %u", op == FILEOP_READ ? "read" : op == FILEOP_DELETE ? "delete" : "write",
            name.c_str(), off, size, buf, buf_size, done);
        st32(fget + FGET_EXC_SIZE, done);
    }
    return cleanup(0);
}

} // namespace

HLE(cellSaveDataUserAutoLoad) { RET(auto_op(ctx, false)); }
HLE(cellSaveDataUserAutoSave) { RET(auto_op(ctx, true)); }

void register_savedata() {
    REG(cellSaveDataUserAutoLoad); REG(cellSaveDataUserAutoSave);
}
