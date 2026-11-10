// AC-3 (Dolby Digital) audio decoder, written from the public ATSC A/52
// standard. Decodes one frame (6 blocks of 256 samples per channel) to
// interleaved stereo float, mixing 5.1 down. Dynamic range compression and
// dither are not applied.
#pragma once
#include <cstddef>
#include <cstdint>

namespace ac3 {

constexpr int FRAME_SAMPLES = 1536;
constexpr int MAX_CHANNELS = 6;   // 5 full-bandwidth channels and LFE

struct Info {
    int sample_rate = 0, frame_bytes = 0, channels = 0;   // channels includes LFE
    int acmod = 0, lfe = 0;
};

// Reads the frame header. False if `data` does not start with an AC-3 frame.
bool parse_header(const uint8_t *data, size_t size, Info &info);

class Decoder {
public:
    Decoder();
    // Decodes one frame into out[FRAME_SAMPLES * 2] (stereo, interleaved).
    // False if the frame is damaged; out then holds silence.
    bool decode(const uint8_t *data, size_t size, float *out);
    const char *error() const { return error_; }   // why the last frame failed

private:
    bool fail(const char *why);
    const char *error_ = nullptr;
    float delay_[MAX_CHANNELS][256] = {};   // second half of the previous block, per channel
};

} // namespace ac3
