// RSX (PS3 GPU) emulation: register file, shader translation, D3D11 backend.
#pragma once
#include "runtime.h"

#include <string>
#include <vector>

namespace rsx {
// Method register file: the last value written to every method address,
// like the hardware's latched state. Indexed by method >> 2.
extern uint32_t regs[0x10000 / 4];
inline uint32_t reg(uint32_t m) { return regs[m >> 2]; }
inline float regf(uint32_t m) { float f; memcpy(&f, &regs[m >> 2], 4); return f; }

// Vertex program memory (512 slots x 4 words) and constants (512 x vec4).
extern uint32_t vp_ucode[512 * 4];
extern float vp_consts[512 * 4];
extern bool vp_consts_dirty;

// Guest addresses for RSX offsets.
uint32_t local_ea(uint32_t off);
uint32_t io_ea(uint32_t off);          // 0 if unmapped
uint32_t ea_for(uint32_t location, uint32_t off); // location: 0 local, 1 main

/* shader translation (vp.cpp / fp.cpp) */
// Constant windows (in vec4 units, multiples of 16): the shader reads c[lo_base ..
// lo_base+lo_count) as cbuffer b0 and c[hi_base .. hi_base+hi_count) as b2 (hi_count 0:
// none). Shaders with relative addressing use the whole 512 in b0.
struct VpInfo { std::string hlsl; uint64_t hash; uint16_t lo_base = 0, lo_count = 512, hi_base = 0, hi_count = 0; };
extern bool g_vp_windows;   // false: always the whole 512 (no D3D11.1 constant buffer offsets)
VpInfo translate_vp(uint32_t start, const uint8_t afmt[16]);

enum TexDim : uint8_t { TEX_2D = 0, TEX_CUBE = 1, TEX_3D = 2 };
constexpr uint8_t TEX_UNNORM = 0x80;  // flag OR-ed into texdim: coordinates are in texels
constexpr uint8_t TEX_SHADOW = 0x40;  // flag: depth texture with a compare function (lookup = filtered depth test)
constexpr uint8_t TEX_QUINCUNX = 0x20; // flag: convolution filter, quincunx kernel (the RSX's AA resolve filter)
constexpr uint8_t TEX_GAUSS = 0x10;    // flag: convolution filter, 3x3 Gaussian kernel
constexpr uint8_t TEX_DIM_MASK = 0x0f;
struct FpKey {
    uint32_t addr;          // guest address of the program
    uint32_t control;       // SET_SHADER_CONTROL
    uint8_t texdim[16];
    uint32_t alpha_func;    // 0 = alpha test off, else GL compare func
    uint16_t remap[16];     // TEXTURE_CONTROL1 channel remap per unit (0xaae4 = identity)
    uint8_t gamma[16];      // TEXTURE_ADDRESS bits 20-23: sRGB-decode channels R=1 G=2 B=4 A=8
    bool srgb_out = false;  // SET_SHADER_PACKER: colour outputs converted linear -> sRGB before blending
};
struct FpInfo { std::string hlsl; uint64_t hash; int mrt_count; };
FpInfo translate_fp(const FpKey &key);
struct FpScan { uint64_t hash; std::vector<uint32_t> consts; uint16_t tex_mask; };  // const offsets from program start; texture units sampled
FpScan scan_fp(uint32_t addr);
uint32_t fp_const_word(uint32_t ea);  // read one halfword-swapped program word

uint64_t fnv(const void *p, size_t n, uint64_t h = 1469598103934665603ull);

/* backend (d3d.cpp) — all called on the FIFO thread */
void init(void *hwnd);
void begin_end(uint32_t prim);
void draw_arrays(uint32_t first, uint32_t count);
void draw_indexed(uint32_t first, uint32_t count);
void clear(uint32_t mask);
void flip(uint32_t display_offset, uint32_t width, uint32_t height, uint32_t pitch);
void copy_m2mf();   // NV0039 memory-to-memory copy
void method(uint32_t m, uint32_t arg);  // state tracking for everything
std::string stats_line(uint32_t frames);   // backend costs since the last call, for the fps log
bool capturing();        // an F12 capture is pending or in progress (for other logs to follow it)
void request_capture();  // F12: log the next frame's draws and dump its render targets
void toggle_cube_maps(); // F11 (diagnostic): cube-map textures read as black
} // namespace rsx
