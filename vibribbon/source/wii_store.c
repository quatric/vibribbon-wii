/* Guest stores from block-timed game code (scripts/inline_stores.py). The
 * store hooks (psx_write_word/half/byte) check lockstep, recording, overlay,
 * cache-isolation, kernel, text-guard and watch hooks before writing; for a
 * store none of them applies to, these write the bytes directly. The checks
 * are psx_store_inline_ok in the runtime's psx_cyc.h. The fallback is a tail
 * call through the CPU's hook, so the fast path needs no stack frame. */
#include <stdint.h>
#include <string.h>
#include "cpu_state.h"

void psx_store_word_direct(CPUState *cpu, uint32_t addr, uint32_t val)
{
    uint32_t phys;
    if (psx_store_inline_ok(addr, 4u, &phys) && phys != 0x00079D44u) {   /* the runtime's D44 probe */
        g_guest_store_count++;
        uint32_t le = __builtin_bswap32(val);
        memcpy(g_psx_ram + phys, &le, 4);
        return;
    }
    cpu->write_word(addr, val);
}

void psx_store_half_direct(CPUState *cpu, uint32_t addr, uint16_t val)
{
    uint32_t phys;
    if (psx_store_inline_ok(addr, 2u, &phys)) {
        g_guest_store_count++;
        uint16_t le = __builtin_bswap16(val);
        memcpy(g_psx_ram + phys, &le, 2);
        return;
    }
    cpu->write_half(addr, val);
}

void psx_store_byte_direct(CPUState *cpu, uint32_t addr, uint8_t val)
{
    uint32_t phys;
    if (psx_store_inline_ok(addr, 1u, &phys)) {
        g_guest_store_count++;
        g_psx_ram[phys] = val;
        return;
    }
    cpu->write_byte(addr, val);
}
