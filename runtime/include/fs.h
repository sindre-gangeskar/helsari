// PS3 filesystem on top of the host filesystem.
//   /dev_bdvd/...  -> the user's disc dump (read-only)
//   /app_home/...  -> <dump>/PS3_GAME/USRDIR
//   /dev_hdd0/..., /dev_hdd1/... -> <data_dir>/dev_hdd0, dev_hdd1 (writable)
#pragma once
#include <cstdint>
#include <string>

namespace fs {
constexpr int32_t ENOENT = (int32_t)0x80010006;
constexpr int32_t EEXIST = (int32_t)0x80010014;
constexpr int32_t EBADF = (int32_t)0x8001002A;
constexpr int32_t EIO = (int32_t)0x8001002B;
constexpr int32_t EINVAL = (int32_t)0x80010002;
constexpr int32_t ENOTDIR = (int32_t)0x80010031;

std::string host_path(const std::string &ps3);
int32_t open(const std::string &path, int32_t flags, uint32_t *fd);
int32_t read(uint32_t fd, uint32_t buf, uint64_t n, uint64_t *got);
int32_t write(uint32_t fd, uint32_t buf, uint64_t n, uint64_t *put);
int32_t close(uint32_t fd);
int32_t lseek(uint32_t fd, int64_t off, int whence, uint64_t *pos);
int32_t stat(const std::string &path, uint32_t out);   // writes CellFsStat
int32_t fstat(uint32_t fd, uint32_t out);
int32_t opendir(const std::string &path, uint32_t *fd);
int32_t readdir(uint32_t fd, uint32_t dirent, uint64_t *nread);
int32_t closedir(uint32_t fd);
int32_t mkdir(const std::string &path);
int32_t rmdir(const std::string &path);
int32_t unlink(const std::string &path);
int32_t rename(const std::string &from, const std::string &to);
}
