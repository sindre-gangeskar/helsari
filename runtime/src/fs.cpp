#include "fs.h"

#include "runtime.h"

#include <cstdio>
#include <filesystem>
#include <map>
#include <mutex>
#include <set>
#include <vector>

namespace stdfs = std::filesystem;

namespace fs {
namespace {
// A guest file. Disc dumps on FAT32 split files over 4 GiB into
// name.66600, name.66601, ...; those are presented as one file.
struct File {
    std::string path;
    std::vector<FILE *> parts;
    std::vector<uint64_t> starts;  // logical offset of each part
    uint64_t size = 0, pos = 0;
};
struct Dir { std::vector<std::pair<std::string, bool>> entries; size_t pos = 0; };

std::mutex g_lock;
std::map<uint32_t, File> g_files;
std::map<uint32_t, Dir> g_dirs;
uint32_t g_next_fd = 3;

constexpr int32_t O_WRONLY = 1, O_RDWR = 2, O_CREAT = 0100, O_EXCL = 0200, O_TRUNC = 01000, O_APPEND = 02000;

bool starts(const std::string &s, const char *p, std::string *rest) {
    size_t n = strlen(p);
    if (s.compare(0, n, p) != 0) return false;
    *rest = s.substr(n);
    return true;
}

std::string part_name(const std::string &h, int i) { return h + ".666" + (i < 10 ? "0" : "") + std::to_string(i); }

// Sizes of the split parts of h, empty if h is not split.
std::vector<uint64_t> split_parts(const std::string &h) {
    std::vector<uint64_t> sizes;
    std::error_code ec;
    for (int i = 0; i < 100; i++) {
        auto p = part_name(h, i);
        if (!stdfs::is_regular_file(p, ec)) break;
        sizes.push_back(stdfs::file_size(p, ec));
    }
    return sizes;
}

void put_stat(uint32_t out, bool dir, uint64_t size) {
    // CellFsStat: mode uid gid atime mtime ctime size blksize (4-byte packed)
    memset(mem::ptr(out), 0, 52);
    st32(out + 0, dir ? 0040777 : 0100666);
    st32(out + 36, (uint32_t)(size >> 32));
    st32(out + 40, (uint32_t)size);
    st32(out + 48, 4096);
}

File *file(uint32_t fd) {
    auto it = g_files.find(fd);
    return it == g_files.end() ? nullptr : &it->second;
}
} // namespace

std::string host_path(const std::string &p) {
    std::string rest;
    if (starts(p, "/dev_bdvd", &rest)) return cfg::dump_dir + rest;
    if (starts(p, "/app_home", &rest)) return cfg::dump_dir + "/PS3_GAME/USRDIR" + rest;
    if (starts(p, "/host_root", &rest)) return cfg::dump_dir + rest;
    for (const char *dev : {"/dev_hdd0", "/dev_hdd1", "/dev_usb000"})
        if (starts(p, dev, &rest)) return cfg::data_dir + dev + rest;
    LOG("fs", "unmapped path '%s'", p.c_str());
    return cfg::data_dir + "/unmapped" + p;
}

// Movies need cellVdec/cellAdec, which are stubs: hide them so the game
// skips playback instead of waiting forever for decoded frames.
// KZ2_PLAY_MOVIES=1 opens them anyway.
bool hidden_movie(const std::string &path) {
    static bool play = getenv("KZ2_PLAY_MOVIES") != nullptr;
    return !play && path.find("/MOVIES/") != std::string::npos;
}

int32_t open(const std::string &path, int32_t flags, uint32_t *fd) {
    if (hidden_movie(path)) { LOG("fs", "open '%s' -> ENOENT (movie skipped)", path.c_str()); return ENOENT; }
    std::string h = host_path(path);
    std::error_code ec;
    File f;
    f.path = path;
    bool exists = stdfs::is_regular_file(h, ec);
    auto parts = exists ? std::vector<uint64_t>{} : split_parts(h);
    if (!parts.empty()) {
        if (flags & (O_WRONLY | O_RDWR)) return EIO;  // split files are disc content: read-only
        uint64_t at = 0;
        for (size_t i = 0; i < parts.size(); i++) {
            f.parts.push_back(fopen(part_name(h, (int)i).c_str(), "rb"));
            f.starts.push_back(at);
            at += parts[i];
        }
        f.size = at;
    } else {
        if ((flags & O_CREAT) && (flags & O_EXCL) && exists) return EEXIST;
        if (!exists && !(flags & O_CREAT)) {
            LOG("fs", "open '%s' -> ENOENT", path.c_str());
            return ENOENT;
        }
        const char *mode = "rb";
        if (flags & (O_WRONLY | O_RDWR)) {
            if (flags & O_APPEND) mode = "ab+";
            else if ((flags & O_TRUNC) || !exists) mode = "wb+";
            else mode = "rb+";
            stdfs::create_directories(stdfs::path(h).parent_path(), ec);
        }
        FILE *fp = fopen(h.c_str(), mode);
        if (!fp) { LOG("fs", "open '%s' failed on host", path.c_str()); return EIO; }
        f.parts.push_back(fp);
        f.starts.push_back(0);
        f.size = exists ? stdfs::file_size(h, ec) : 0;
    }
    std::lock_guard<std::mutex> lk(g_lock);
    *fd = g_next_fd++;
    LOG("fs", "open '%s' flags=0x%x -> fd %u (%llu bytes%s)", path.c_str(), flags, *fd,
        (unsigned long long)f.size, f.parts.size() > 1 ? ", split" : "");
    g_files[*fd] = std::move(f);
    return 0;
}

int32_t read(uint32_t fd, uint32_t buf, uint64_t n, uint64_t *got) {
    std::lock_guard<std::mutex> lk(g_lock);
    File *f = file(fd);
    if (!f) return EBADF;
    uint64_t done = 0;
    while (done < n && f->pos < f->size) {
        size_t i = f->parts.size() - 1;
        while (i && f->starts[i] > f->pos) i--;
        uint64_t part_end = i + 1 < f->parts.size() ? f->starts[i + 1] : f->size;
        uint64_t chunk = std::min(n - done, part_end - f->pos);
        _fseeki64(f->parts[i], (int64_t)(f->pos - f->starts[i]), SEEK_SET);
        mem::ensure(buf + (uint32_t)done, (uint32_t)chunk);
        char what[160];
        snprintf(what, sizeof what, "read '%s' at 0x%llx", f->path.c_str(), (unsigned long long)f->pos);
        mem::watch_host_write(buf + (uint32_t)done, (uint32_t)chunk, what);
        size_t r = fread(mem::ptr(buf + (uint32_t)done), 1, (size_t)chunk, f->parts[i]);
        if (mem::hook_watched(buf + (uint32_t)done, (uint32_t)chunk))
            LOG("hook", "fs %s: 0x%zx of 0x%llx bytes -> 0x%x, first %08x %08x", what, r, (unsigned long long)chunk,
                buf + (uint32_t)done, ld32(buf + (uint32_t)done), ld32(buf + (uint32_t)done + 4));
        done += r, f->pos += r;
        if (r < chunk) break;
    }
    *got = done;
    return 0;
}

int32_t write(uint32_t fd, uint32_t buf, uint64_t n, uint64_t *put) {
    std::lock_guard<std::mutex> lk(g_lock);
    File *f = file(fd);
    if (!f) return EBADF;
    if (f->parts.size() != 1) return EIO;
    _fseeki64(f->parts[0], (int64_t)f->pos, SEEK_SET);
    mem::ensure(buf, (uint32_t)n);
    *put = fwrite(mem::ptr(buf), 1, (size_t)n, f->parts[0]);
    fflush(f->parts[0]);
    f->pos += *put;
    if (f->pos > f->size) f->size = f->pos;
    return 0;
}

int32_t close(uint32_t fd) {
    std::lock_guard<std::mutex> lk(g_lock);
    File *f = file(fd);
    if (!f) return EBADF;
    for (FILE *p : f->parts) fclose(p);
    g_files.erase(fd);
    return 0;
}

int32_t lseek(uint32_t fd, int64_t off, int whence, uint64_t *pos) {
    std::lock_guard<std::mutex> lk(g_lock);
    File *f = file(fd);
    if (!f) return EBADF;
    int64_t base = whence == 1 ? (int64_t)f->pos : whence == 2 ? (int64_t)f->size : 0;
    if (base + off < 0) return EINVAL;
    f->pos = (uint64_t)(base + off);
    *pos = f->pos;
    return 0;
}

int32_t stat(const std::string &path, uint32_t out) {
    if (hidden_movie(path)) return ENOENT;
    std::string h = host_path(path);
    std::error_code ec;
    if (stdfs::is_directory(h, ec)) { put_stat(out, true, 0); return 0; }
    if (stdfs::is_regular_file(h, ec)) { put_stat(out, false, stdfs::file_size(h, ec)); return 0; }
    auto parts = split_parts(h);
    if (!parts.empty()) {
        uint64_t total = 0;
        for (auto s : parts) total += s;
        put_stat(out, false, total);
        return 0;
    }
    LOG("fs", "stat '%s' -> ENOENT", path.c_str());
    return ENOENT;
}

int32_t fstat(uint32_t fd, uint32_t out) {
    std::lock_guard<std::mutex> lk(g_lock);
    File *f = file(fd);
    if (!f) return EBADF;
    put_stat(out, false, f->size);
    return 0;
}

int32_t opendir(const std::string &path, uint32_t *fd) {
    std::string h = host_path(path);
    std::error_code ec;
    if (!stdfs::is_directory(h, ec)) { LOG("fs", "opendir '%s' -> ENOENT", path.c_str()); return ENOENT; }
    Dir d;
    d.entries.push_back({".", true});
    d.entries.push_back({"..", true});
    std::set<std::string> joined;
    for (auto &e : stdfs::directory_iterator(h, ec)) {
        std::string name = e.path().filename().string();
        size_t dot = name.rfind(".666");
        if (dot != std::string::npos && dot + 6 == name.size()) {  // show split parts as one file
            if (!joined.insert(name.substr(0, dot)).second) continue;
            name = name.substr(0, dot);
        }
        d.entries.push_back({name, e.is_directory()});
    }
    std::lock_guard<std::mutex> lk(g_lock);
    *fd = g_next_fd++;
    g_dirs[*fd] = std::move(d);
    return 0;
}

int32_t readdir(uint32_t fd, uint32_t dirent, uint64_t *nread) {
    std::lock_guard<std::mutex> lk(g_lock);
    auto it = g_dirs.find(fd);
    if (it == g_dirs.end()) return EBADF;
    Dir &d = it->second;
    if (d.pos >= d.entries.size()) { *nread = 0; return 0; }
    auto &e = d.entries[d.pos++];
    // CellFsDirent: u8 d_type, u8 d_namlen, char d_name[256]
    st8(dirent, e.second ? 1 : 2);
    st8(dirent + 1, (uint8_t)e.first.size());
    mem::write_cstr(dirent + 2, e.first, 256);
    *nread = 258;
    return 0;
}

int32_t closedir(uint32_t fd) {
    std::lock_guard<std::mutex> lk(g_lock);
    return g_dirs.erase(fd) ? 0 : EBADF;
}

int32_t mkdir(const std::string &path) {
    std::error_code ec;
    std::string h = host_path(path);
    if (stdfs::exists(h, ec)) return EEXIST;
    stdfs::create_directories(h, ec);
    return ec ? EIO : 0;
}
int32_t rmdir(const std::string &path) {
    std::error_code ec;
    return stdfs::remove(host_path(path), ec) ? 0 : ENOENT;
}
int32_t unlink(const std::string &path) {
    std::error_code ec;
    return stdfs::remove(host_path(path), ec) ? 0 : ENOENT;
}
int32_t rename(const std::string &a, const std::string &b) {
    std::error_code ec;
    stdfs::rename(host_path(a), host_path(b), ec);
    return ec ? ENOENT : 0;
}
} // namespace fs
