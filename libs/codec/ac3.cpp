// AC-3 decoder, following the decoding process of ATSC A/52 (section numbers
// below refer to it). The tables are the standard's.
#include "ac3.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace ac3 {
namespace {

/* ------------------------------ tables (A/52) ------------------------------ */

// Frame size in 16-bit words at 32, 44.1 and 48 kHz, by frmsizecod / 2.
const int BITRATES[19] = {32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 384, 448, 512, 576, 640};
const int SAMPLE_RATES[3] = {48000, 44100, 32000};

// Bit allocation (7.2.2).
const uint8_t BAND_START[51] = {0,  1,  2,  3,  4,  5,  6,  7,  8,  9,  10, 11,  12,  13,  14,  15,  16,
                                17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28,  31,  34,  37,  40,  43,
                                46, 49, 55, 61, 67, 73, 79, 85, 97, 109, 121, 133, 157, 181, 205, 229, 253};
const uint8_t LOG_ADD[260] = {
    0x40, 0x3f, 0x3e, 0x3d, 0x3c, 0x3b, 0x3a, 0x39, 0x38, 0x37, 0x36, 0x35, 0x34, 0x34, 0x33, 0x32, 0x31, 0x30, 0x2f, 0x2f,
    0x2e, 0x2d, 0x2c, 0x2c, 0x2b, 0x2a, 0x29, 0x29, 0x28, 0x27, 0x26, 0x26, 0x25, 0x24, 0x24, 0x23, 0x23, 0x22, 0x21, 0x21,
    0x20, 0x20, 0x1f, 0x1e, 0x1e, 0x1d, 0x1d, 0x1c, 0x1c, 0x1b, 0x1b, 0x1a, 0x1a, 0x19, 0x19, 0x18, 0x18, 0x17, 0x17, 0x16,
    0x16, 0x15, 0x15, 0x15, 0x14, 0x14, 0x13, 0x13, 0x13, 0x12, 0x12, 0x12, 0x11, 0x11, 0x11, 0x10, 0x10, 0x10, 0x0f, 0x0f,
    0x0f, 0x0e, 0x0e, 0x0e, 0x0d, 0x0d, 0x0d, 0x0d, 0x0c, 0x0c, 0x0c, 0x0c, 0x0b, 0x0b, 0x0b, 0x0b, 0x0a, 0x0a, 0x0a, 0x0a,
    0x0a, 0x09, 0x09, 0x09, 0x09, 0x09, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x07, 0x07, 0x07, 0x07, 0x07, 0x07, 0x06, 0x06,
    0x06, 0x06, 0x06, 0x06, 0x06, 0x06, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04,
    0x04, 0x04, 0x04, 0x04, 0x04, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x02,
    0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x01, 0x01,
    0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
    0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};
const uint16_t HEARING_THRESHOLD[50][3] = {
    {0x04d0, 0x04f0, 0x0580}, {0x04d0, 0x04f0, 0x0580}, {0x0440, 0x0460, 0x04b0}, {0x0400, 0x0410, 0x0450},
    {0x03e0, 0x03e0, 0x0420}, {0x03c0, 0x03d0, 0x03f0}, {0x03b0, 0x03c0, 0x03e0}, {0x03b0, 0x03b0, 0x03d0},
    {0x03a0, 0x03b0, 0x03c0}, {0x03a0, 0x03a0, 0x03b0}, {0x03a0, 0x03a0, 0x03b0}, {0x03a0, 0x03a0, 0x03b0},
    {0x03a0, 0x03a0, 0x03a0}, {0x0390, 0x03a0, 0x03a0}, {0x0390, 0x0390, 0x03a0}, {0x0390, 0x0390, 0x03a0},
    {0x0380, 0x0390, 0x03a0}, {0x0380, 0x0380, 0x03a0}, {0x0370, 0x0380, 0x03a0}, {0x0370, 0x0380, 0x03a0},
    {0x0360, 0x0370, 0x0390}, {0x0360, 0x0370, 0x0390}, {0x0350, 0x0360, 0x0390}, {0x0350, 0x0360, 0x0390},
    {0x0340, 0x0350, 0x0380}, {0x0340, 0x0350, 0x0380}, {0x0330, 0x0340, 0x0380}, {0x0320, 0x0340, 0x0370},
    {0x0310, 0x0320, 0x0360}, {0x0300, 0x0310, 0x0350}, {0x02f0, 0x0300, 0x0340}, {0x02f0, 0x02f0, 0x0330},
    {0x02f0, 0x02f0, 0x0320}, {0x02f0, 0x02f0, 0x0310}, {0x0300, 0x02f0, 0x0300}, {0x0310, 0x0300, 0x02f0},
    {0x0340, 0x0320, 0x02f0}, {0x0390, 0x0350, 0x02f0}, {0x03e0, 0x0390, 0x0300}, {0x0420, 0x03e0, 0x0310},
    {0x0460, 0x0420, 0x0330}, {0x0490, 0x0450, 0x0350}, {0x04a0, 0x04a0, 0x03c0}, {0x0460, 0x0490, 0x0410},
    {0x0440, 0x0460, 0x0470}, {0x0440, 0x0440, 0x04a0}, {0x0520, 0x0480, 0x0460}, {0x0800, 0x0630, 0x0440},
    {0x0840, 0x0840, 0x0450}, {0x0840, 0x0840, 0x04e0},
};
const uint8_t BAP_TAB[64] = {0,  1,  1,  1,  1,  1,  2,  2,  3,  3,  3,  4,  4,  5,  5,  6,  6,  6,  6,  7,  7,  7,
                             7,  8,  8,  8,  8,  9,  9,  9,  9,  10, 10, 10, 10, 11, 11, 11, 11, 12, 12, 12, 12, 13,
                             13, 13, 13, 14, 14, 14, 14, 14, 14, 14, 14, 15, 15, 15, 15, 15, 15, 15, 15, 15};
const int SLOW_DECAY[4] = {0x0f, 0x11, 0x13, 0x15};
const int FAST_DECAY[4] = {0x3f, 0x53, 0x67, 0x7b};
const int SLOW_GAIN[4] = {0x540, 0x4d8, 0x478, 0x410};
const int DB_PER_BIT[4] = {0x000, 0x700, 0x900, 0xb00};
const int FLOOR[8] = {0x2f0, 0x2b0, 0x270, 0x230, 0x1f0, 0x170, 0x0f0, -0x800};   // 0xf800 as a signed 16-bit value
const int FAST_GAIN[8] = {0x080, 0x100, 0x180, 0x200, 0x280, 0x300, 0x380, 0x400};

// Bits per mantissa for the asymmetric quantizers, bap 6..15 (7.3.3).
const int QUANT_BITS[16] = {0, 0, 0, 3, 0, 4, 5, 6, 7, 8, 9, 10, 11, 12, 14, 16};

// Full-bandwidth channels per audio coding mode, and the downmix levels (5.4.2.4, 5.4.2.5).
const int FULL_CHANNELS[8] = {2, 1, 2, 3, 3, 4, 4, 5};
const float CENTRE_MIX[4] = {0.707f, 0.595f, 0.500f, 0.595f};
const float SURROUND_MIX[4] = {0.707f, 0.500f, 0.000f, 0.500f};

uint8_t band_of_bin[256];   // which bit allocation band each frequency bin is in
float window_[512];         // Kaiser-Bessel-derived window, alpha 5
float cos_long[256][512];   // inverse transform kernels
float cos_short[2][128][256];

double bessel_i0(double x) {
    double sum = 1, term = 1;
    for (int k = 1; k < 50; k++) {
        term *= (x / (2 * k)) * (x / (2 * k));
        sum += term;
    }
    return sum;
}

void init_tables() {
    static bool done;
    if (done) return;
    done = true;
    for (int b = 0; b < 50; b++)
        for (int i = BAND_START[b]; i < BAND_START[b + 1]; i++) band_of_bin[i] = (uint8_t)b;
    band_of_bin[253] = band_of_bin[254] = band_of_bin[255] = 49;
    // KBD window (7.9.4.1): cumulative Kaiser window of 257 points.
    const double pi = 3.14159265358979323846, alpha = 5.0;
    double kaiser[257], total = 0;
    for (int j = 0; j <= 256; j++) {
        const double r = (j - 128.0) / 128.0;
        kaiser[j] = bessel_i0(pi * alpha * std::sqrt(std::max(0.0, 1 - r * r)));
        total += kaiser[j];
    }
    double run = 0;
    for (int n = 0; n < 256; n++) {
        run += kaiser[n];
        window_[n] = window_[511 - n] = (float)std::sqrt(run / total);
    }
    // Long block: 256 coefficients -> 512 samples, cos(pi/256 (n + 1/2 + 128)(k + 1/2)).
    for (int k = 0; k < 256; k++)
        for (int n = 0; n < 512; n++) cos_long[k][n] = (float)std::cos(pi / 256 * (n + 0.5 + 128) * (k + 0.5));
    // Short blocks: two transforms of 128 coefficients; the first has no phase shift, the
    // second is shifted by half its length (alpha -1 and +1 in the standard's formula).
    for (int k = 0; k < 128; k++)
        for (int n = 0; n < 256; n++) {
            cos_short[0][k][n] = (float)std::cos(pi / 128 * (n + 0.5) * (k + 0.5));
            cos_short[1][k][n] = (float)std::cos(pi / 128 * (n + 0.5 + 128) * (k + 0.5));
        }
}

/* --------------------------------- bit reader --------------------------------- */

struct Bits {
    const uint8_t *p;
    size_t size, pos = 0;   // in bits
    bool over = false;
    uint32_t get(int n) {
        uint32_t v = 0;
        for (int i = 0; i < n; i++) {
            const size_t byte = pos >> 3;
            if (byte >= size) { over = true; return 0; }
            v = v << 1 | ((p[byte] >> (7 - (pos & 7))) & 1);
            pos++;
        }
        return v;
    }
    int sget(int n) {   // two's complement
        const uint32_t v = get(n);
        return (int)(v << (32 - n)) >> (32 - n);
    }
};

/* ------------------------------ bit allocation ------------------------------ */

struct Delta {
    int mode = 2;   // 0 reuse, 1 new, 2 none
    int segments = 0, offset[8] = {}, length[8] = {}, value[8] = {};
};

int log_add(int a, int b) {
    const int c = a - b, i = std::min(std::abs(c) >> 1, 255);
    return c >= 0 ? a + LOG_ADD[i] : b + LOG_ADD[i];
}

int low_comp(int a, int b0, int b1, int bin) {
    if (bin < 7) {
        if (b0 + 256 == b1) a = 384;
        else if (b0 > b1) a = std::max(0, a - 64);
    } else if (bin < 20) {
        if (b0 + 256 == b1) a = 320;
        else if (b0 > b1) a = std::max(0, a - 64);
    } else
        a = std::max(0, a - 128);
    return a;
}

struct AllocParams {
    int fscod, sdecay, fdecay, sgain, dbknee, floor;
};

// 7.2.2: bit allocation pointers for bins [start, end) from their exponents.
void allocate(const AllocParams &g, const uint8_t *exp, int start, int end, int fgain, int snroffset, bool lfe,
              bool coupling, int fleak, int sleak, const Delta &delta, uint8_t *bap) {
    int psd[256], bndpsd[50], excite[50], mask[50];
    for (int i = start; i < end; i++) psd[i] = 3072 - (exp[i] << 7);
    int j = start, k = band_of_bin[start], lastbin;
    do {
        lastbin = std::min((int)BAND_START[k + 1], end);
        bndpsd[k] = psd[j++];
        for (; j < lastbin; j++) bndpsd[k] = log_add(bndpsd[k], psd[j]);
        k++;
    } while (end > lastbin);

    const int bndstrt = band_of_bin[start], bndend = band_of_bin[end - 1] + 1;
    int begin, fastleak = 0, slowleak = 0;
    if (!coupling) {
        int lowcomp = low_comp(0, bndpsd[0], bndpsd[1], 0);
        excite[0] = bndpsd[0] - fgain - lowcomp;
        lowcomp = low_comp(lowcomp, bndpsd[1], bndpsd[2], 1);
        excite[1] = bndpsd[1] - fgain - lowcomp;
        begin = 7;
        for (int bin = 2; bin < 7; bin++) {
            if (!lfe || bin != 6) lowcomp = low_comp(lowcomp, bndpsd[bin], bndpsd[bin + 1], bin);
            fastleak = bndpsd[bin] - fgain;
            slowleak = bndpsd[bin] - g.sgain;
            excite[bin] = fastleak - lowcomp;
            if ((!lfe || bin != 6) && bndpsd[bin] <= bndpsd[bin + 1]) {
                begin = bin + 1;
                break;
            }
        }
        for (int bin = begin; bin < std::min(bndend, 22); bin++) {
            if (!lfe || bin != 6) lowcomp = low_comp(lowcomp, bndpsd[bin], bndpsd[bin + 1], bin);
            fastleak = std::max(fastleak - g.fdecay, bndpsd[bin] - fgain);
            slowleak = std::max(slowleak - g.sdecay, bndpsd[bin] - g.sgain);
            excite[bin] = std::max(fastleak - lowcomp, slowleak);
        }
        begin = 22;
    } else {
        begin = bndstrt;
        fastleak = (fleak << 8) + 768;
        slowleak = (sleak << 8) + 768;
    }
    for (int bin = begin; bin < bndend; bin++) {
        fastleak = std::max(fastleak - g.fdecay, bndpsd[bin] - fgain);
        slowleak = std::max(slowleak - g.sdecay, bndpsd[bin] - g.sgain);
        excite[bin] = std::max(fastleak, slowleak);
    }
    for (int bin = bndstrt; bin < bndend; bin++) {
        if (bndpsd[bin] < g.dbknee) excite[bin] += (g.dbknee - bndpsd[bin]) >> 2;
        mask[bin] = std::max(excite[bin], (int)HEARING_THRESHOLD[bin][g.fscod]);
    }
    if (delta.mode == 0 || delta.mode == 1) {
        int band = 0;
        for (int seg = 0; seg <= delta.segments; seg++) {
            band += delta.offset[seg];
            const int d = delta.value[seg] >= 4 ? (delta.value[seg] - 3) << 7 : (delta.value[seg] - 4) << 7;
            for (int n = 0; n < delta.length[seg] && band < 50; n++) mask[band++] += d;
        }
    }
    int i = start;
    j = band_of_bin[start];
    do {
        lastbin = std::min((int)BAND_START[j + 1], end);
        int m = mask[j] - snroffset - g.floor;
        if (m < 0) m = 0;
        m = (m & 0x1fe0) + g.floor;
        for (; i < lastbin; i++) bap[i] = BAP_TAB[std::clamp((psd[i] - m) >> 5, 0, 63)];
        j++;
    } while (end > lastbin);
}

/* --------------------------------- mantissas --------------------------------- */

// Grouped quantizers (bap 1, 2, 4) pack 3, 3 and 2 values per code; the groups
// run on across channels within an audio block (7.3.5).
struct Mantissas {
    float g1[3], g2[3], g4[2];
    int n1 = 0, n2 = 0, n4 = 0;
    float read(Bits &b, int bap) {
        switch (bap) {
        case 0: return 0;
        case 1:
            if (!n1) {
                const int c = (int)b.get(5);
                g1[0] = (c / 9 - 1) * (2.0f / 3), g1[1] = (c / 3 % 3 - 1) * (2.0f / 3), g1[2] = (c % 3 - 1) * (2.0f / 3);
                n1 = 3;
            }
            return g1[3 - n1--];
        case 2:
            if (!n2) {
                const int c = (int)b.get(7);
                g2[0] = (c / 25 - 2) * 0.4f, g2[1] = (c / 5 % 5 - 2) * 0.4f, g2[2] = (c % 5 - 2) * 0.4f;
                n2 = 3;
            }
            return g2[3 - n2--];
        case 3: return (int)(b.get(3) * 2 - 6) / 7.0f;
        case 4:
            if (!n4) {
                const int c = (int)b.get(7);
                g4[0] = (c / 11 * 2 - 10) / 11.0f, g4[1] = (c % 11 * 2 - 10) / 11.0f;
                n4 = 2;
            }
            return g4[2 - n4--];
        case 5: return (int)(b.get(4) * 2 - 14) / 15.0f;
        default: {
            const int bits = QUANT_BITS[bap];
            return b.sget(bits) / (float)(1 << (bits - 1));
        }
        }
    }
};

// Exponents (7.1.3): an absolute first value, then 7-bit groups of three
// differences, each applied to 1, 2 or 4 bins (strategy 1, 2, 3).
bool read_exponents(Bits &b, int strategy, int groups, int absexp, uint8_t *out, int first_bin, bool first_is_ref) {
    const int size = 1 << (strategy - 1);
    int prev = absexp, bin = first_bin;
    if (!first_is_ref) out[bin++] = (uint8_t)absexp;
    for (int g = 0; g < groups; g++) {
        const int c = (int)b.get(7);
        const int d[3] = {c / 25 - 2, c % 25 / 5 - 2, c % 5 - 2};
        for (int k = 0; k < 3; k++) {
            prev += d[k];
            if (prev < 0 || prev > 24) return false;
            for (int r = 0; r < size && bin < 256; r++) out[bin++] = (uint8_t)prev;
        }
    }
    return true;
}

} // namespace

/* ------------------------------------ frame ------------------------------------ */

bool parse_header(const uint8_t *d, size_t size, Info &info) {
    if (size < 7 || d[0] != 0x0b || d[1] != 0x77) return false;
    const int fscod = d[4] >> 6, frmsizecod = d[4] & 0x3f;
    if (fscod == 3 || frmsizecod >= 38 || (d[5] >> 3) > 10) return false;
    const int rate = BITRATES[frmsizecod >> 1];
    info.sample_rate = SAMPLE_RATES[fscod];
    info.frame_bytes = fscod == 0 ? rate * 4 : fscod == 2 ? rate * 6 : (rate * 320 / 147 + (frmsizecod & 1)) * 2;
    Bits b{d, size};
    b.pos = 40;
    b.get(5), b.get(3);   // bsid, bsmod
    info.acmod = (int)b.get(3);
    if ((info.acmod & 1) && info.acmod != 1) b.get(2);
    if (info.acmod & 4) b.get(2);
    if (info.acmod == 2) b.get(2);
    info.lfe = (int)b.get(1);
    info.channels = FULL_CHANNELS[info.acmod] + info.lfe;
    return true;
}

Decoder::Decoder() { init_tables(); }

bool Decoder::fail(const char *why) {
    error_ = why;
    return false;
}

bool Decoder::decode(const uint8_t *data, size_t size, float *out) {
    std::fill(out, out + FRAME_SAMPLES * 2, 0.0f);
    error_ = nullptr;
    Info info;
    if (!parse_header(data, size, info) || (size_t)info.frame_bytes > size) return fail("no frame header");
    Bits b{data, (size_t)info.frame_bytes};
    b.pos = 32;
    const int fscod = (int)b.get(2);
    b.get(6);
    // Bit stream information (5.4.2).
    b.get(5), b.get(3);
    const int acmod = (int)b.get(3), nfchans = FULL_CHANNELS[acmod];
    int cmixlev = 0, surmixlev = 0;
    if ((acmod & 1) && acmod != 1) cmixlev = (int)b.get(2);
    if (acmod & 4) surmixlev = (int)b.get(2);
    if (acmod == 2) b.get(2);
    const int lfeon = (int)b.get(1);
    for (int pass = 0; pass < (acmod == 0 ? 2 : 1); pass++) {
        b.get(5);                        // dialnorm
        if (b.get(1)) b.get(8);          // compr
        if (b.get(1)) b.get(8);          // langcod
        if (b.get(1)) b.get(5), b.get(2);   // mixlevel, roomtyp
    }
    b.get(1), b.get(1);   // copyrightb, origbs
    if (b.get(1)) b.get(14);
    if (b.get(1)) b.get(14);
    if (b.get(1)) {
        const int n = (int)b.get(6) + 1;
        for (int i = 0; i < n; i++) b.get(8);
    }

    // State carried from block to block within the frame.
    const int lfe_ch = nfchans;   // index of the LFE channel
    uint8_t exps[MAX_CHANNELS][256] = {}, cplexps[256] = {}, bap[MAX_CHANNELS][256] = {}, cplbap[256] = {};
    int endmant[MAX_CHANNELS] = {}, chbwcod[MAX_CHANNELS] = {};
    bool cplinu = false, chincpl[MAX_CHANNELS] = {}, phsflginu = false;
    int cplbegf = 0, cplendf = 0, ncplbnd = 0, cplstrtmant = 0, cplendmant = 0;
    int cplbnd_end[18] = {};   // end bin of each coupling band
    float cplco[MAX_CHANNELS][18] = {};
    bool phsflg[18] = {};
    int rematflg[4] = {};
    AllocParams g{fscod, 0, 0, 0, 0, 0};
    int csnroffst = 0, fsnroffst[MAX_CHANNELS] = {}, fgaincod[MAX_CHANNELS] = {}, cplfsnroffst = 0, cplfgaincod = 0;
    int cplfleak = 0, cplsleak = 0;
    Delta delta[MAX_CHANNELS], cpldelta;
    bool have_alloc = false;
    float coef[MAX_CHANNELS][256];
    float pcm[MAX_CHANNELS][FRAME_SAMPLES];

    for (int blk = 0; blk < 6; blk++) {
        // Audio block (5.4.3).
        bool blksw[MAX_CHANNELS] = {};
        for (int ch = 0; ch < nfchans; ch++) blksw[ch] = b.get(1);
        for (int ch = 0; ch < nfchans; ch++) b.get(1);   // dithflag: dither is not applied
        for (int pass = 0; pass < (acmod == 0 ? 2 : 1); pass++)
            if (b.get(1)) b.get(8);   // dynrng: not applied
        if (b.get(1)) {               // cplstre
            cplinu = b.get(1);
            if (cplinu) {
                for (int ch = 0; ch < nfchans; ch++) chincpl[ch] = b.get(1);
                if (acmod == 2) phsflginu = b.get(1);
                cplbegf = (int)b.get(4);
                cplendf = (int)b.get(4);
                const int nsub = 3 + cplendf - cplbegf;
                if (nsub < 1) return fail("coupling bands");
                ncplbnd = 0;
                for (int s = 0; s < nsub; s++) {
                    const bool merge = s > 0 && b.get(1);
                    if (!merge) ncplbnd++;
                    cplbnd_end[ncplbnd - 1] = cplbegf * 12 + 37 + (s + 1) * 12;
                }
                cplstrtmant = cplbegf * 12 + 37;
                cplendmant = (cplendf + 3) * 12 + 37;
            } else
                for (int ch = 0; ch < nfchans; ch++) chincpl[ch] = false;
        } else if (blk == 0)
            return fail("no coupling strategy in block 0");
        if (cplinu) {
            bool any = false;
            for (int ch = 0; ch < nfchans; ch++) {
                if (!chincpl[ch] || !b.get(1)) continue;   // cplcoe
                any = true;
                const int mstr = (int)b.get(2) * 3;
                for (int bnd = 0; bnd < ncplbnd; bnd++) {
                    const int e = (int)b.get(4), m = (int)b.get(4);
                    const float mant = e == 15 ? m / 16.0f : (m + 16) / 32.0f;
                    cplco[ch][bnd] = std::ldexp(mant, -(e + mstr));
                }
            }
            if (acmod == 2 && phsflginu && any)
                for (int bnd = 0; bnd < ncplbnd; bnd++) phsflg[bnd] = b.get(1);
        }
        if (acmod == 2 && b.get(1)) {   // rematstr
            const int n = !cplinu || cplbegf > 2 ? 4 : cplbegf > 0 ? 3 : 2;
            for (int r = 0; r < 4; r++) rematflg[r] = r < n ? (int)b.get(1) : 0;
        }
        // Exponent strategies and bandwidth.
        const int cplexpstr = cplinu ? (int)b.get(2) : 0;
        int chexpstr[MAX_CHANNELS] = {};
        for (int ch = 0; ch < nfchans; ch++) chexpstr[ch] = (int)b.get(2);
        const int lfeexpstr = lfeon ? (int)b.get(1) : 0;
        for (int ch = 0; ch < nfchans; ch++)
            if (chexpstr[ch] && !chincpl[ch]) {
                chbwcod[ch] = (int)b.get(6);
                if (chbwcod[ch] > 60) return fail("bandwidth code");
            }
        if (blk == 0 && (!lfeexpstr && lfeon)) return fail("no LFE exponents in block 0");
        // Exponents.
        if (cplinu && cplexpstr) {
            const int absexp = (int)b.get(4) << 1, groups = (cplendmant - cplstrtmant) / (3 << (cplexpstr - 1));
            if (!read_exponents(b, cplexpstr, groups, absexp, cplexps, cplstrtmant, true)) return fail("coupling exponents");
        }
        for (int ch = 0; ch < nfchans; ch++) {
            endmant[ch] = chincpl[ch] ? cplstrtmant : (chbwcod[ch] + 12) * 3 + 37;
            if (!chexpstr[ch]) continue;
            const int e = endmant[ch];
            const int groups = chexpstr[ch] == 1 ? (e - 1) / 3 : chexpstr[ch] == 2 ? (e + 2) / 6 : (e + 8) / 12;
            if (!read_exponents(b, chexpstr[ch], groups, (int)b.get(4), exps[ch], 0, false)) return fail("channel exponents");
            b.get(2);   // gainrng
        }
        if (lfeon) {
            endmant[lfe_ch] = 7;
            if (lfeexpstr && !read_exponents(b, 1, 2, (int)b.get(4), exps[lfe_ch], 0, false)) return fail("LFE exponents");
        }
        // Bit allocation parameters.
        if (b.get(1)) {   // baie
            g.sdecay = SLOW_DECAY[b.get(2)];
            g.fdecay = FAST_DECAY[b.get(2)];
            g.sgain = SLOW_GAIN[b.get(2)];
            g.dbknee = DB_PER_BIT[b.get(2)];
            g.floor = FLOOR[b.get(3)];
            have_alloc = true;
        }
        if (b.get(1)) {   // snroffste
            csnroffst = (int)b.get(6);
            if (cplinu) cplfsnroffst = (int)b.get(4), cplfgaincod = (int)b.get(3);
            for (int ch = 0; ch < nfchans; ch++) fsnroffst[ch] = (int)b.get(4), fgaincod[ch] = (int)b.get(3);
            if (lfeon) fsnroffst[lfe_ch] = (int)b.get(4), fgaincod[lfe_ch] = (int)b.get(3);
        }
        if (cplinu && b.get(1)) cplfleak = (int)b.get(3), cplsleak = (int)b.get(3);
        if (b.get(1)) {   // deltbaie
            if (cplinu) cpldelta.mode = (int)b.get(2);
            for (int ch = 0; ch < nfchans; ch++) delta[ch].mode = (int)b.get(2);
            auto read_delta = [&](Delta &dl) {
                dl.segments = (int)b.get(3);
                for (int s = 0; s <= dl.segments; s++)
                    dl.offset[s] = (int)b.get(5), dl.length[s] = (int)b.get(4), dl.value[s] = (int)b.get(3);
            };
            if (cplinu && cpldelta.mode == 1) read_delta(cpldelta);
            for (int ch = 0; ch < nfchans; ch++)
                if (delta[ch].mode == 1) read_delta(delta[ch]);
        }
        if (b.get(1)) {   // skiple
            const int n = (int)b.get(9);
            for (int i = 0; i < n; i++) b.get(8);
        }
        if (!have_alloc) return fail("no bit allocation parameters");
        // Bit allocation.
        auto snr = [&](int fine) { return (((csnroffst - 15) << 4) + fine) << 2; };
        for (int ch = 0; ch < nfchans; ch++)
            allocate(g, exps[ch], 0, endmant[ch], FAST_GAIN[fgaincod[ch]], snr(fsnroffst[ch]), false, false, 0, 0,
                     delta[ch], bap[ch]);
        if (cplinu)
            allocate(g, cplexps, cplstrtmant, cplendmant, FAST_GAIN[cplfgaincod], snr(cplfsnroffst), false, true, cplfleak,
                     cplsleak, cpldelta, cplbap);
        if (lfeon) {
            Delta none;
            allocate(g, exps[lfe_ch], 0, 7, FAST_GAIN[fgaincod[lfe_ch]], snr(fsnroffst[lfe_ch]), true, false, 0, 0, none,
                     bap[lfe_ch]);
        }
        // Mantissas, then coefficients = mantissa * 2^-exponent.
        Mantissas m;
        float cplcoef[256] = {};
        bool got_cpl = false;
        for (int ch = 0; ch < nfchans; ch++) {
            std::fill(coef[ch], coef[ch] + 256, 0.0f);
            for (int bin = 0; bin < endmant[ch]; bin++)
                coef[ch][bin] = std::ldexp(m.read(b, bap[ch][bin]), -exps[ch][bin]);
            if (cplinu && chincpl[ch] && !got_cpl) {
                for (int bin = cplstrtmant; bin < cplendmant; bin++)
                    cplcoef[bin] = std::ldexp(m.read(b, cplbap[bin]), -cplexps[bin]);
                got_cpl = true;
            }
        }
        if (lfeon) {
            std::fill(coef[lfe_ch], coef[lfe_ch] + 256, 0.0f);
            for (int bin = 0; bin < 7; bin++) coef[lfe_ch][bin] = std::ldexp(m.read(b, bap[lfe_ch][bin]), -exps[lfe_ch][bin]);
        }
        if (b.over) return fail("frame data ran out");
        // Decoupling (7.4).
        if (cplinu)
            for (int ch = 0; ch < nfchans; ch++) {
                if (!chincpl[ch]) continue;
                int bnd = 0;
                for (int bin = cplstrtmant; bin < cplendmant; bin++) {
                    while (bin >= cplbnd_end[bnd]) bnd++;
                    float v = cplcoef[bin] * cplco[ch][bnd] * 8;
                    if (acmod == 2 && ch == 1 && phsflginu && phsflg[bnd]) v = -v;
                    coef[ch][bin] = v;
                }
            }
        // Rematrixing (7.5).
        if (acmod == 2) {
            static const int REMAT[5] = {13, 25, 37, 61, 253};
            const int end = std::min(endmant[0], endmant[1]);
            for (int r = 0; r < 4; r++) {
                if (!rematflg[r]) continue;
                for (int bin = REMAT[r]; bin < std::min(REMAT[r + 1], end); bin++) {
                    const float l = coef[0][bin], rr = coef[1][bin];
                    coef[0][bin] = l + rr, coef[1][bin] = l - rr;
                }
            }
        }
        // Inverse transform, window and overlap (7.9).
        const int nch = nfchans + lfeon;
        for (int ch = 0; ch < nch; ch++) {
            float y[512] = {};
            if (ch < nfchans && blksw[ch]) {
                for (int half = 0; half < 2; half++)
                    for (int k = 0; k < 128; k++) {
                        const float x = coef[ch][2 * k + half];
                        if (x == 0) continue;
                        const float *c = cos_short[half][k];
                        float *o = y + 256 * half;
                        for (int n = 0; n < 256; n++) o[n] += x * c[n];
                    }
            } else {
                for (int k = 0; k < 256; k++) {
                    const float x = coef[ch][k];
                    if (x == 0) continue;
                    const float *c = cos_long[k];
                    for (int n = 0; n < 512; n++) y[n] += x * c[n];
                }
            }
            float *o = pcm[ch] + blk * 256;
            for (int n = 0; n < 256; n++) {
                o[n] = -2 * (y[n] * window_[n]) + delay_[ch][n];
                delay_[ch][n] = -2 * (y[n + 256] * window_[n + 256]);
            }
        }
    }

    // Downmix to stereo (7.8), without the LFE channel.
    const float c = CENTRE_MIX[cmixlev], s = SURROUND_MIX[surmixlev];
    for (int i = 0; i < FRAME_SAMPLES; i++) {
        float l = 0, r = 0;
        switch (acmod) {
        case 0: l = pcm[0][i], r = pcm[1][i]; break;
        case 1: l = r = pcm[0][i] * 0.707f; break;
        case 2: l = pcm[0][i], r = pcm[1][i]; break;
        case 3: l = pcm[0][i] + c * pcm[1][i], r = pcm[2][i] + c * pcm[1][i]; break;
        case 4: l = pcm[0][i] + s * 0.707f * pcm[2][i], r = pcm[1][i] + s * 0.707f * pcm[2][i]; break;
        case 5: l = pcm[0][i] + c * pcm[1][i] + s * 0.707f * pcm[3][i], r = pcm[2][i] + c * pcm[1][i] + s * 0.707f * pcm[3][i]; break;
        case 6: l = pcm[0][i] + s * pcm[2][i], r = pcm[1][i] + s * pcm[3][i]; break;
        case 7: l = pcm[0][i] + c * pcm[1][i] + s * pcm[3][i], r = pcm[2][i] + c * pcm[1][i] + s * pcm[4][i]; break;
        }
        const float norm = acmod >= 3 ? 1.0f / (1 + c + (acmod >= 4 ? s : 0)) : 1.0f;
        out[2 * i] = l * norm, out[2 * i + 1] = r * norm;
    }
    return true;
}

} // namespace ac3
