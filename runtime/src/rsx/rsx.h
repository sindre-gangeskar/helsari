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
struct VpInfo { std::string hlsl; uint64_t hash; };
VpInfo translate_vp(uint32_t start);

enum TexDim : uint8_t { TEX_2D = 0, TEX_CUBE = 1, TEX_3D = 2 };
constexpr uint8_t TEX_UNNORM = 0x80;  // flag OR-ed into texdim: coordinates are in texels
constexpr uint8_t TEX_SHADOW = 0x40;  // flag: depth texture with a compare function (lookup = filtered depth test)
constexpr uint8_t TEX_DIM_MASK = 0x3f;
struct FpKey {
    uint32_t addr;          // guest address of the program
    uint32_t control;       // SET_SHADER_CONTROL
    uint8_t texdim[16];
    uint32_t alpha_func;    // 0 = alpha test off, else GL compare func
};
struct FpInfo { std::string hlsl; uint64_t hash; int mrt_count; };
FpInfo translate_fp(const FpKey &key);
struct FpScan { uint64_t hash; std::vector<uint32_t> consts; };  // const offsets from program start
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
void request_capture();  // F12: log the next frame's draws and dump its render targets
} // namespace rsx
