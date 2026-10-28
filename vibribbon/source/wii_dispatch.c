/* The boot loader and MAIN_G.EXE are recompiled as two programs whose dispatch
 * tables export the same entry points. The Makefile renames each program's
 * copies (boot_*, main_*); these combine them. */
#include <stdint.h>
#include "cpu_state.h"

#define COMBINE_INT(name, args, call) \
    extern int boot_##name args; extern int main_##name args; \
    int name args { return boot_##name call || main_##name call; }

COMBINE_INT(psx_dispatch_game_compiled, (CPUState *cpu, uint32_t addr), (cpu, addr))
COMBINE_INT(psx_game_address_in_text, (uint32_t addr), (addr))
COMBINE_INT(psx_game_is_function_entry, (uint32_t addr), (addr))
COMBINE_INT(psx_game_text_native_ok, (uint32_t addr), (addr))
COMBINE_INT(psx_game_text_native_ok_full, (uint32_t addr), (addr))
