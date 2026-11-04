// Native menu entries: the game's menus are text pages (SVML layout + Lua) inside data_ps3.psarc.
// The dump stays untouched and no game data is stored: when the archive is opened, the pages to
// change are read from it and patched in memory, then appended past its real end, and reads of
// its table of contents are redirected there. KZ2_MENU_PATCH=0: no changes.
#include "runtime.h"

#include <algorithm>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

long inflate_raw(const uint8_t *src, size_t srclen, uint8_t *dst, size_t dstlen); // inflate.cpp

namespace menu_patch
{
    namespace
    {

        constexpr uint32_t HEADER_SIZE = 32, ENTRY_NAMES = 0;

        std::vector<uint8_t> g_appended;
        uint64_t g_real_size;
        struct Patch
        {
            uint64_t at;
            std::vector<uint8_t> bytes;
        };
        std::vector<Patch> g_toc_patches;

        uint64_t be(const uint8_t *p, int n)
        {
            uint64_t v = 0;
            while (n--)
                v = v << 8 | *p++;
            return v;
        }
        void put_be(uint8_t *p, uint64_t v, int n)
        {
            while (n--)
            {
                p[n] = (uint8_t)v;
                v >>= 8;
            }
        }

        // A zlib stream holding `d` uncompressed: the game's reader inflates it like any other block.
        std::vector<uint8_t> stored_block(const std::string &d)
        {
            std::vector<uint8_t> z = {0x78, 0xda, 0x01, (uint8_t)d.size(), (uint8_t)(d.size() >> 8),
                                      (uint8_t)~d.size(), (uint8_t)(~d.size() >> 8)};
            z.insert(z.end(), d.begin(), d.end());
            uint32_t a = 1, b = 0;
            for (unsigned char c : d)
                a = (a + c) % 65521, b = (b + a) % 65521;
            for (int i = 3; i >= 0; i--)
                z.push_back((uint8_t)((b << 16 | a) >> (8 * i)));
            return z;
        }

        // The game loads its pages from these caches, one per language; the .jsp files are not read.
        bool is_page_cache(const std::string &name) { return name.find("cachedjspingamesp.core") != std::string::npos; }

        // The GRAPHICS and GAMEPLAY lists for the options page. pc_options.cpp hides them when the page
        // opens, adds the section entries, shows the values and takes the rows' clicks. Short names and
        // no label text, since the room here is small (see patch_page): the labels are set when shown.
        const char *const GRAPHICS_LIST =
            R"x(<_l name="lG" c="_Normal" p="16,14,620,203" a="true" bn="DOWN_BUTTON" bp="UP_BUTTON" g="900" /> )x"
            R"x(<_i name="iF" c="_Medium" p=",,620,29" g="901" h="static://hs/fps" /> <_t name="tF" c="_Medium" >-</_t> <_i: name="!" g="901" /> )x"
            R"x(<_i name="iR" c="_Medium" p=",29,620,29" g="902" h="static://hs/res" /> <_t name="tR" c="_Medium" >-</_t> <_i: name="!" g="902" /> )x"
            R"x(<_i name="iM" c="_Medium" p=",58,620,29" g="903" h="static://hs/mode" /> <_t name="tM" c="_Medium" >-</_t> <_i: name="!" g="903" /> )x"
            R"x(<_i name="iV" c="_Medium" p=",87,620,29" g="904" h="static://hs/vsync" /> <_t name="tV" c="_Medium" >-</_t> <_i: name="!" g="904" /> )x"
            R"x(<_l: name="!" g="900" /> )x"
            R"x(<_l name="lP" c="_Normal" p="16,14,620,203" a="true" bn="DOWN_BUTTON" bp="UP_BUTTON" g="905" /> )x"
            R"x(<_i name="iS" c="_Medium" p=",,620,29" g="906" h="static://hs/sway" /> <_t name="tS" c="_Medium" >-</_t> <_i: name="!" g="906" /> )x"
            R"x(<_l: name="!" g="905" /> )x";

        // What to change in a page cache. Returns false if it is left alone. A page must keep its exact
        // length (growing one crashed the game), so what we add is paid for by turning the script's CRLF
        // line ends into LF, which does not change what the script means.
        bool patch_page(std::string &f)
        {
            const size_t id = f.find("<PAGEID name=\"kin-preparsed_menu_campaign_options\"");
            if (id == std::string::npos)
                return false;
            const size_t row = f.find("<_l name=\"listControlSettings\"", id);
            const size_t page_end = f.find("</SVML>", id);
            if (row == std::string::npos || page_end == std::string::npos || row > page_end)
                return false;
            const size_t tag = f.find("<_s name=\"optionsmenuscript\"", row);
            if (tag == std::string::npos || tag > page_end)
                return false;
            const std::string add = GRAPHICS_LIST;
            size_t need = add.size(), have = 0;
            for (size_t i = tag; i + 1 < page_end; i++)
                have += f[i] == '\r' && f[i + 1] == '\n';
            if (have < need)
            {
                LOG("menu", "not enough line ends to free (%zu of %zu)", have, need);
                return false;
            }
            // The script text, with line ends freed: from its start, as many as needed.
            std::string body;
            body.reserve(page_end - tag);
            for (size_t i = tag; i < page_end; i++)
            {
                if (need && f[i] == '\r' && f[i + 1] == '\n')
                {
                    need--;
                    continue;
                }
                body += f[i];
            }
            const std::string between = f.substr(row, tag - row);
            f.replace(row, page_end - row, add + between + body);
            return true;
        }

    } // namespace

    // rd reads from the real archive at a logical offset.
    void build(uint64_t real_size, const std::function<void(uint64_t, void *, size_t)> &rd)
    {
        static bool done;
        if (done)
            return;
        done = true;
        g_real_size = real_size;
        if (getenv("KZ2_MENU_PATCH") && !atoi(getenv("KZ2_MENU_PATCH")))
            return;
        uint8_t h[HEADER_SIZE];
        rd(0, h, sizeof h);
        if (memcmp(h, "PSAR", 4) || memcmp(h + 8, "zlib", 4))
        {
            LOG("menu", "not a zlib PSARC: no patches");
            return;
        }
        const uint32_t toc = (uint32_t)be(h + 12, 4), esz = (uint32_t)be(h + 16, 4), n = (uint32_t)be(h + 20, 4),
                       block = (uint32_t)be(h + 24, 4);
        if (esz != 30 || block != 65536)
        {
            LOG("menu", "unexpected PSARC layout (entry %u, block %u)", esz, block);
            return;
        }
        std::vector<uint8_t> entries(esz * n);
        rd(HEADER_SIZE, entries.data(), entries.size());
        const uint64_t zl_at = HEADER_SIZE + (uint64_t)esz * n;
        const uint32_t nz = (uint32_t)((toc - zl_at) / 2);
        std::vector<uint8_t> zl(nz * 2);
        rd(zl_at, zl.data(), zl.size());

        auto extract = [&](uint32_t i, std::string &out)
        {
            const uint8_t *e = &entries[i * esz];
            uint32_t zi = (uint32_t)be(e + 16, 4);
            uint64_t size = be(e + 20, 5), off = be(e + 25, 5);
            out.resize(size);
            std::vector<uint8_t> raw;
            for (uint64_t have = 0; have < size; zi++)
            {
                uint32_t z = (uint32_t)be(&zl[zi * 2], 2), want = (uint32_t)std::min<uint64_t>(size - have, block);
                raw.resize(z ? z : block);
                rd(off, raw.data(), raw.size());
                off += raw.size();
                if (z && raw[0] == 0x78)
                {
                    if (inflate_raw(raw.data() + 2, raw.size() - 2, (uint8_t *)&out[have], want) != (long)want)
                        return false;
                }
                else
                {
                    memcpy(&out[have], raw.data(), want);
                }
                have += want;
            }
            return true;
        };

        std::string names;
        if (!extract(ENTRY_NAMES, names))
        {
            LOG("menu", "cannot read the PSARC file list");
            return;
        }
        uint32_t index = 1, changed = 0;
        for (size_t p = 0; p < names.size() && index < n; index++)
        {
            size_t e = names.find('\n', p);
            if (e == std::string::npos)
                e = names.size();
            std::string name = names.substr(p, e - p);
            p = e + 1;
            std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c)
                           { return (char)tolower(c); });
            if (!is_page_cache(name))
                continue;
            std::string file;
            if (!extract(index, file))
            {
                LOG("menu", "cannot read %s", name.c_str());
                continue;
            }
            const uint8_t *entry = &entries[index * esz];
            const uint64_t old_size = be(entry + 20, 5);
            if (!patch_page(file))
                continue;
            // The block table cannot grow, so the file must keep its block count. Full blocks are
            // stored raw (table length 0), the last one as a zlib stream.
            const uint64_t blocks = (file.size() + block - 1) / block, last = file.size() - (blocks - 1) * block;
            if (blocks != (old_size + block - 1) / block || last > 65500)
            {
                LOG("menu", "%s: patched file does not fit its blocks (%zu bytes)", name.c_str(), file.size());
                continue;
            }
            const uint32_t zi = (uint32_t)be(entry + 16, 4);
            const uint64_t at = g_real_size + g_appended.size();
            g_appended.insert(g_appended.end(), file.begin(), file.begin() + (size_t)((blocks - 1) * block));
            const std::vector<uint8_t> z = stored_block(file.substr((size_t)((blocks - 1) * block)));
            g_appended.insert(g_appended.end(), z.begin(), z.end());
            std::vector<uint8_t> fields(10);
            put_be(&fields[0], file.size(), 5);
            put_be(&fields[5], at, 5);
            g_toc_patches.push_back({HEADER_SIZE + (uint64_t)index * esz + 20, fields});
            std::vector<uint8_t> lens((size_t)blocks * 2); // all zero: raw full blocks
            put_be(&lens[lens.size() - 2], z.size(), 2);
            g_toc_patches.push_back({zl_at + (uint64_t)zi * 2, lens});
            LOG("menu", "patched %s (%zu bytes, %llu blocks, entry %u)", name.c_str(), file.size(), (unsigned long long)blocks, index);
            changed++;
        }
        LOG("menu", "%u menu pages patched", changed);
    }

    uint64_t extra_size() { return g_appended.size(); }
    uint64_t real_size() { return g_real_size; }

    // Copies appended bytes for a read at or past the real end.
    size_t read_appended(uint64_t pos, void *dst, size_t n)
    {
        uint64_t off = pos - g_real_size;
        if (off >= g_appended.size())
            return 0;
        n = (size_t)std::min<uint64_t>(n, g_appended.size() - off);
        memcpy(dst, &g_appended[off], n);
        return n;
    }

    // Applies table-of-contents changes to data just read from the real archive.
    void apply(uint64_t pos, uint8_t *data, size_t n)
    {
        for (auto &p : g_toc_patches)
        {
            uint64_t from = std::max(pos, p.at), to = std::min(pos + n, p.at + p.bytes.size());
            if (from < to)
                memcpy(data + (from - pos), &p.bytes[from - p.at], to - from);
        }
    }

} // namespace menu_patch
