// Native menu entries: the game's menus are text pages (SVML layout + Lua) inside data_ps3.psarc.
// The dump stays untouched and no game data is stored: when the archive is opened, the pages to
// change are read from it and patched in memory, then appended past its real end, and reads of
// its table of contents are redirected there. HELSARI_MENU_PATCH=0: no changes.
#include "keybinds.h"
#include "runtime.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <functional>
#include <map>
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
        uint64_t be(const uint8_t *p, int n);
        void put_be(uint8_t *p, uint64_t v, int n);
        // Update patches we applied ourselves, by lowercase file name: the game gets a patch that changes nothing.
        std::map<std::string, std::vector<uint8_t>> g_replaced;

        // The update changes archive files with deltas in USRDIR/patch_data_ps3, applied by the game as it
        // reads them. If one changes a page cache we patch, the delta would land on our bytes, so we apply it
        // first and patch the result. Our reading of the format, all big-endian 32-bit words:
        //   op table size, literal size, then the op table: output size, then tokens. A token with the top bit
        //   set is a copy (output offset | top bit, source offset); any other is a pair (end of the copy
        //   before, offset into the literals). The gaps between copies take the literal bytes in order. A
        //   copy followed by a copy ends where the next starts; the last one ends at the output size.
        //   The literal bytes follow the op table.
        struct Delta
        {
            uint32_t out = 0;
            std::vector<uint32_t> d, s, e; // the copies
            std::vector<uint8_t> lit;
        };

        bool read_delta(const std::vector<uint8_t> &b, Delta &x)
        {
            if (b.size() < 12)
                return false;
            const uint32_t opn = (uint32_t)be(&b[0], 4), litn = (uint32_t)be(&b[4], 4);
            if (opn < 4 || opn % 4 || 8ull + opn + litn > b.size())
                return false;
            const uint32_t words = opn / 4;
            auto w = [&](uint32_t i)
            { return (uint32_t)be(&b[8 + i * 4], 4); };
            x.out = w(0);
            for (uint32_t i = 1; i + 1 < words; i += 2)
            {
                if (w(i) & 0x80000000u)
                {
                    if (!x.e.empty() && x.e.back() == UINT32_MAX)
                        x.e.back() = w(i) & 0x7fffffffu;
                    x.d.push_back(w(i) & 0x7fffffffu), x.s.push_back(w(i + 1)), x.e.push_back(UINT32_MAX);
                }
                else if (!x.e.empty() && x.e.back() == UINT32_MAX)
                    x.e.back() = w(i);
            }
            if (!x.e.empty() && x.e.back() == UINT32_MAX)
                x.e.back() = x.out;
            x.lit.assign(b.begin() + 8 + opn, b.begin() + 8 + opn + litn);
            return true;
        }

        // The source size a delta was made for: its last copy reaches the end of the source.
        uint64_t delta_source_size(const Delta &x)
        {
            return x.d.empty() ? 0 : (uint64_t)x.s.back() + (x.e.back() - x.d.back());
        }

        bool apply_delta(const Delta &x, const std::string &src, std::string &out)
        {
            out.assign(x.out, '\0');
            size_t pos = 0, lp = 0;
            auto fill = [&](size_t to)
            {
                if (to < pos || to > x.out || lp + (to - pos) > x.lit.size())
                    return false;
                memcpy(&out[pos], x.lit.data() + lp, to - pos);
                lp += to - pos, pos = to;
                return true;
            };
            for (size_t i = 0; i < x.d.size(); i++)
            {
                if (!fill(x.d[i]) || x.e[i] < x.d[i] || x.e[i] > x.out || (uint64_t)x.s[i] + (x.e[i] - x.d[i]) > src.size())
                    return false;
                memcpy(&out[x.d[i]], src.data() + x.s[i], x.e[i] - x.d[i]);
                pos = x.e[i];
            }
            return fill(x.out) && lp == x.lit.size();
        }

        std::vector<uint8_t> read_host(const std::filesystem::path &p)
        {
            std::vector<uint8_t> b;
            if (FILE *f = _wfopen(p.c_str(), L"rb"))
            {
                fseek(f, 0, SEEK_END);
                b.resize((size_t)ftell(f));
                fseek(f, 0, SEEK_SET);
                b.resize(fread(b.data(), 1, b.size(), f));
                fclose(f);
            }
            return b;
        }

        // The update's deltas by the source size they apply to, read once.
        std::multimap<uint64_t, std::filesystem::path> &deltas_by_source_size()
        {
            static std::multimap<uint64_t, std::filesystem::path> m;
            static bool done;
            if (done)
                return m;
            done = true;
            if (cfg::update_dir.empty())
                return m;
            std::error_code ec;
            for (const auto &it : std::filesystem::directory_iterator(cfg::update_dir + "/USRDIR/patch_data_ps3", ec))
            {
                Delta x;
                if (it.path().extension() == ".pspatch" && read_delta(read_host(it.path()), x))
                    m.emplace(delta_source_size(x), it.path());
            }
            LOG("menu", "%zu update patches indexed", m.size());
            return m;
        }

        // Applies the update's delta for this file, if it has one. Returns the delta's lowercase file name.
        std::string apply_update(std::string &file)
        {
            const auto range = deltas_by_source_size().equal_range(file.size());
            for (auto it = range.first; it != range.second; ++it)
            {
                Delta x;
                std::string out;
                if (!read_delta(read_host(it->second), x) || !apply_delta(x, file, out) || out.compare(0, 7, "RTTIBin"))
                    continue;
                file.swap(out);
                std::string name = it->second.filename().string();
                std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c)
                               { return (char)tolower(c); });
                return name;
            }
            return {};
        }

        // A delta that copies its whole source: output size n, one copy from 0.
        std::vector<uint8_t> identity_delta(uint32_t n)
        {
            std::vector<uint8_t> b(20);
            put_be(&b[0], 12, 4);
            put_be(&b[8], n, 4);
            put_be(&b[12], 0x80000000u, 4);
            return b;
        }

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

        // The GRAPHICS, GAMEPLAY and KEYBINDINGS lists for the options page. pc_options.cpp hides them
        // when the page opens, adds the section entries, shows the values and takes the rows' clicks.
        // Short names and no label text, since room is short (see patch_page): the labels are set when shown.
        std::string pc_lists()
        {
            const auto row = [](const std::string &n, int y, int g, const std::string &url, int h)
            {
                const std::string id = std::to_string(g);
                return "<_i name=\"i" + n + "\" c=\"_Medium\" p=\"," + std::to_string(y) + ",620," + std::to_string(h) + "\" g=\"" + id + "\" h=\"static://hs/" + url +
                       "\" /> <_t name=\"t" + n + "\" c=\"_Medium\">-</_t> <_i: name=\"!\" g=\"" + id + "\" /> ";
            };
            const auto list = [](const char *name, int height, int g)
            {
                return std::string("<_l name=\"") + name + "\" c=\"_Normal\" p=\"16,14,620," + std::to_string(height) + "\" a=\"true\" bn=\"DOWN_BUTTON\" bp=\"UP_BUTTON\" g=\"" + std::to_string(g) + "\" /> ";
            };
            const auto end = [](int g)
            { return "<_l: name=\"!\" g=\"" + std::to_string(g) + "\" /> "; };
            // A row with a slider after its text, as the game's own volume rows have: m and x are its range, s
            // its step, u its first value, d and i the buttons that move it.
            const auto slider_row = [](const std::string &n, int y, int g, const std::string &url, int h, int lo, int hi)
            {
                const std::string id = std::to_string(g);
                return "<_i name=\"i" + n + "\" c=\"_Medium\" p=\"," + std::to_string(y) + ",620," + std::to_string(h) + "\" g=\"" + id + "\" h=\"static://hs/" + url +
                       "\" /> <_t name=\"t" + n + "\" c=\"_Medium\">-</_t> <_r name=\"s" + n + "\" c=\"_SliderMedium\" p=\"370,,220,\" a=\"true\" d=\"LEFT_BUTTON\" i=\"RIGHT_BUTTON\" m=\"" +
                       std::to_string(lo) + "\" s=\"1\" x=\"" + std::to_string(hi) + "\" /> <_i: name=\"!\" g=\"" + id + "\" /> ";
            };
            // GRAPHICS: one row per line. To reorder, move the lines and renumber y (0, 29, 58, ...) from the top.
            //          name  y    group action   height
            std::string out = list("lG", 203, 900);
            out += row("R", 0, 901, "res", 29);                   // RESOLUTION
            out += row("M", 29, 902, "mode", 29);                 // WINDOW MODE
            out += row("V", 58, 903, "vsync", 29);                // VSYNC
            out += row("F", 87, 904, "fps", 29);                  // FRAMERATE LIMIT
            out += slider_row("O", 116, 907, "fov", 29, 73, 110); // FIELD OF VIEW (slider: 73 to 110)
            out += row("B", 145, 908, "fx", 29);                  // SCREEN FX
            out += row("A", 174, 930, "api", 29);                 // RENDERING API (group 930: 911-924 are the keys)
            out += end(900);
            out += list("lP", 203, 905) + row("S", 0, 906, "sway", 29) + row("I", 29, 909, "intro", 29) + end(905); // SWAY, INTRO MOVIE
            constexpr int ROW = 26;
            out += list("lK", KEYBIND_COUNT * ROW, 910);
            for (int i = 0; i < KEYBIND_COUNT; i++)
                out += row("K" + std::to_string(i), i * ROW, 911 + i, "k" + std::to_string(i), ROW);
            return out + end(910);
        }

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
            std::string add = pc_lists();
            // Room first from text nothing reads: the language table's select_button, page_title and
            // caption fields (no script in the cache reads them, and the executable never names them),
            // then from CRLF line ends, which does not change what the script means.
            std::string body = f.substr(tag, page_end - tag);
            size_t need = add.size();
            for (const char *field : {"select_button = \"", "page_title = \"", "caption = \""})
                for (size_t at = body.find(field); need && at != std::string::npos; at = body.find(field, at))
                {
                    const size_t quote = body.find('"', at + strlen(field));
                    if (quote == std::string::npos)
                        break;
                    size_t from = at, to = quote + 1;
                    if (body.compare(to, 2, ", ") == 0)
                        to += 2;
                    else if (from >= 2 && body.compare(from - 2, 2, ", ") == 0)
                        from -= 2;
                    const size_t size = to - from;
                    body.erase(from, size);
                    need = size >= need ? 0 : need - size;
                    at = from;
                    if (size > add.size() + need) // never: a field is far smaller than the lists
                        break;
                }
            for (size_t i = 0; need && i + 1 < body.size(); i++)
                if (body[i] == '\r' && body[i + 1] == '\n')
                {
                    body.erase(i, 1);
                    need--;
                }
            if (need)
            {
                LOG("menu", "not enough room in the options page: %zu bytes short", need);
                return false;
            }
            // A removed field can free more than was needed: spaces between tags keep the length.
            const size_t total = add.size() + body.size();
            if (total < page_end - tag)
                add += std::string(page_end - tag - total, ' ');
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
        if (getenv("HELSARI_MENU_PATCH") && !atoi(getenv("HELSARI_MENU_PATCH")))
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
            const std::string delta = apply_update(file);
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
            if (!delta.empty())
                g_replaced[delta] = identity_delta((uint32_t)file.size());
            LOG("menu", "patched %s (%zu bytes, %llu blocks, entry %u%s%s)", name.c_str(), file.size(), (unsigned long long)blocks,
                index, delta.empty() ? "" : ", update patch ", delta.c_str());
            changed++;
        }
        LOG("menu", "%u menu pages patched", changed);
    }

    // What the game reads instead of an update patch we already applied, or null.
    const std::vector<uint8_t> *replaced_update_patch(const std::string &path)
    {
        std::string name = path.substr(path.find_last_of('/') + 1);
        std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c)
                       { return (char)tolower(c); });
        const auto it = g_replaced.find(name);
        return it == g_replaced.end() ? nullptr : &it->second;
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
