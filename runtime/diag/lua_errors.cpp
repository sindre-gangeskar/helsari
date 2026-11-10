// Lua runtime errors, logged before the game reports them.
//
// The game's report of a Lua type error ("attempt to index a nil value") crashes in this runtime, with a
// state pointer that is no longer a pointer. This hook runs first: it logs the state, the operation, the
// type of the value and the guest call stack, so the cause can be read from the log.
#include "runtime.h"

extern "C" {
void f_003f7ba8_guest(PPU_CTX); // the type error: (L, the offending value, the operation as text)
}

extern "C" void f_003f7ba8(PPU_CTX)
{
    static int logs;
    if (logs++ < 20)
    {
        const uint32_t L = (uint32_t)ctx->r[3], value = (uint32_t)ctx->r[4], op = (uint32_t)ctx->r[5];
        const bool l_ok = L >= 0x10000 && L < 0xf0000000u;
        std::string what = op >= 0x10000 && op < 0xf0000000u ? (const char *)mem::ptr(op) : "?";
        const uint32_t tag = value >= 0x10000 && value < 0xf0000000u ? ld32(value + 4) : 0xffffffffu;
        LOG("lua", "type error: attempt to %s a value of type %u; state %08x (%s), caller %08x", what.c_str(), tag, L,
            l_ok ? "looks valid" : "NOT A POINTER", (uint32_t)ctx->lr);
        // Guest call stack: each frame links to the caller's, whose return address is at +16.
        std::string stack;
        uint32_t sp = (uint32_t)ctx->r[1];
        for (int i = 0; i < 12 && sp >= 0x10000 && sp < 0xf0000000u; i++)
        {
            const uint32_t caller = (uint32_t)ld64(sp);
            if (!caller || caller <= sp)
                break;
            char b[16];
            snprintf(b, sizeof b, " %x", (uint32_t)ld64(caller + 16));
            stack += b;
            sp = caller;
        }
        LOG("lua", "  guest stack:%s", stack.c_str());
        LOG("lua", "  r14..r31: %llx %llx %llx %llx %llx %llx %llx %llx %llx %llx %llx %llx %llx %llx %llx %llx %llx %llx",
            (unsigned long long)ctx->r[14], (unsigned long long)ctx->r[15], (unsigned long long)ctx->r[16], (unsigned long long)ctx->r[17],
            (unsigned long long)ctx->r[18], (unsigned long long)ctx->r[19], (unsigned long long)ctx->r[20], (unsigned long long)ctx->r[21],
            (unsigned long long)ctx->r[22], (unsigned long long)ctx->r[23], (unsigned long long)ctx->r[24], (unsigned long long)ctx->r[25],
            (unsigned long long)ctx->r[26], (unsigned long long)ctx->r[27], (unsigned long long)ctx->r[28], (unsigned long long)ctx->r[29],
            (unsigned long long)ctx->r[30], (unsigned long long)ctx->r[31]);
    }
    f_003f7ba8_guest(ctx);
}
