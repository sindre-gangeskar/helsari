// Raw DEFLATE (RFC 1951) decoder, used in place of the Edge Zlib SPU task.
#include <cstdint>
#include <cstring>

namespace {
struct Huff {
    uint16_t count[16];
    uint16_t sym[288];
};

struct In {
    const uint8_t *p, *end;
    uint32_t bits = 0;
    int n = 0;
    bool bad = false;
    int bit() {
        if (!n) {
            if (p >= end) { bad = true; return 0; }
            bits = *p++, n = 8;
        }
        int b = bits & 1;
        bits >>= 1, n--;
        return b;
    }
    uint32_t get(int k) {
        uint32_t v = 0;
        for (int i = 0; i < k; i++) v |= (uint32_t)bit() << i;
        return v;
    }
};

void build(Huff &h, const uint8_t *lens, int n) {
    uint16_t offs[16];
    memset(h.count, 0, sizeof h.count);
    for (int i = 0; i < n; i++) h.count[lens[i]]++;
    h.count[0] = 0;
    offs[1] = 0;
    for (int i = 1; i < 15; i++) offs[i + 1] = offs[i] + h.count[i];
    for (int i = 0; i < n; i++)
        if (lens[i]) h.sym[offs[lens[i]]++] = (uint16_t)i;
}

int decode(In &in, const Huff &h) {
    int code = 0, first = 0, index = 0;
    for (int len = 1; len < 16; len++) {
        code |= in.bit();
        int c = h.count[len];
        if (code - c < first) return h.sym[index + (code - first)];
        index += c, first += c;
        first <<= 1, code <<= 1;
        if (in.bad) return -1;
    }
    return -1;
}

const uint16_t LBASE[] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
const uint8_t LEXT[] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
const uint16_t DBASE[] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
const uint8_t DEXT[] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
} // namespace

// Returns the number of bytes written, or -1 on corrupt input / overflow.
long inflate_raw(const uint8_t *src, size_t srclen, uint8_t *dst, size_t dstlen) {
    In in{src, src + srclen};
    size_t out = 0;
    int last;
    do {
        last = in.bit();
        int type = (int)in.get(2);
        if (type == 0) {  // stored
            in.n = 0;
            if (in.end - in.p < 4) return -1;
            uint16_t len = (uint16_t)(in.p[0] | in.p[1] << 8);
            in.p += 4;
            if (in.end - in.p < len || out + len > dstlen) return -1;
            memcpy(dst + out, in.p, len);
            in.p += len, out += len;
            continue;
        }
        Huff lit, dist;
        uint8_t lens[320];
        if (type == 1) {  // fixed
            int i = 0;
            for (; i < 144; i++) lens[i] = 8;
            for (; i < 256; i++) lens[i] = 9;
            for (; i < 280; i++) lens[i] = 7;
            for (; i < 288; i++) lens[i] = 8;
            build(lit, lens, 288);
            for (i = 0; i < 30; i++) lens[i] = 5;
            build(dist, lens, 30);
        } else if (type == 2) {  // dynamic
            int nlen = (int)in.get(5) + 257, ndist = (int)in.get(5) + 1, ncode = (int)in.get(4) + 4;
            static const uint8_t order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
            uint8_t cl[19] = {0};
            for (int i = 0; i < ncode; i++) cl[order[i]] = (uint8_t)in.get(3);
            Huff ch;
            build(ch, cl, 19);
            int i = 0;
            while (i < nlen + ndist) {
                int s = decode(in, ch);
                if (s < 0) return -1;
                if (s < 16) { lens[i++] = (uint8_t)s; continue; }
                int rep, val = 0;
                if (s == 16) { if (!i) return -1; val = lens[i - 1]; rep = 3 + (int)in.get(2); }
                else if (s == 17) rep = 3 + (int)in.get(3);
                else rep = 11 + (int)in.get(7);
                if (i + rep > nlen + ndist) return -1;
                while (rep--) lens[i++] = (uint8_t)val;
            }
            build(lit, lens, nlen);
            build(dist, lens + nlen, ndist);
        } else {
            return -1;
        }
        for (;;) {
            int s = decode(in, lit);
            if (s < 0) return -1;
            if (s < 256) {
                if (out >= dstlen) return -1;
                dst[out++] = (uint8_t)s;
            } else if (s == 256) {
                break;
            } else {
                s -= 257;
                if (s >= 29) return -1;
                size_t len = LBASE[s] + in.get(LEXT[s]);
                int ds = decode(in, dist);
                if (ds < 0 || ds >= 30) return -1;
                size_t d = DBASE[ds] + in.get(DEXT[ds]);
                if (d > out || out + len > dstlen) return -1;
                for (size_t k = 0; k < len; k++, out++) dst[out] = dst[out - d];
            }
        }
        if (in.bad) return -1;
    } while (!last);
    return (long)out;
}
