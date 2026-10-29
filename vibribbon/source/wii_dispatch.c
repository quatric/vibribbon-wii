/* The boot loader and the main executables it chains to (MAIN_G game, MAIN_T
 * title) are recompiled as separate programs whose dispatch tables export the
 * same entry points. The Makefile renames each program's copies (boot_*,
 * main_*, title_*); these combine them.
 *
 * MAIN_G and MAIN_T load at the same address, so each has its own reference
 * text for the runtime's native-code guard. The program that matched last is
 * tried first, with its image selected; both images include the boot loader. */
#include <stdint.h>
#include "cpu_state.h"
#include "wii_host.h"

extern void dirty_ram_select_text_image(uint32_t phys_lo, const uint8_t *bytes, uint32_t len);

#define PROG_DECLS(p) \
    extern int p##_psx_dispatch_game_compiled(CPUState *cpu, uint32_t addr); \
    extern int p##_psx_game_address_in_text(uint32_t addr); \
    extern int p##_psx_game_is_function_entry(uint32_t addr); \
    extern int p##_psx_game_text_native_ok(uint32_t addr); \
    extern int p##_psx_game_text_native_ok_full(uint32_t addr);
PROG_DECLS(boot)
PROG_DECLS(main)
PROG_DECLS(title)

typedef struct {
    int (*dispatch)(CPUState *cpu, uint32_t addr);
    int (*in_text)(uint32_t addr);
    int (*is_entry)(uint32_t addr);
    int (*ok)(uint32_t addr);
    int (*ok_full)(uint32_t addr);
    const uint8_t *img;
    uint32_t lo, len;
} MainProg;

#define PROG(p) { p##_psx_dispatch_game_compiled, p##_psx_game_address_in_text, \
                  p##_psx_game_is_function_entry, p##_psx_game_text_native_ok, \
                  p##_psx_game_text_native_ok_full, 0, 0, 0 }
static MainProg s_prog[WII_MAIN_PROGS] = { PROG(main), PROG(title) };
static int s_cur;   /* program whose reference image is active */

void wii_dispatch_set_text(int prog, uint32_t lo, const uint8_t *img, uint32_t len)
{
    s_prog[prog].img = img;
    s_prog[prog].lo = lo;
    s_prog[prog].len = len;
}

static void use(int k)
{
    if (k != s_cur && s_prog[k].img) {
        dirty_ram_select_text_image(s_prog[k].lo, s_prog[k].img, s_prog[k].len);
        s_cur = k;
    }
}

/* The boot loader's functions all lie in its own text, below the main
 * programs, so an address is either the boot loader's or a main program's:
 * one range test saves a table search on every check. */
int psx_dispatch_game_compiled(CPUState *cpu, uint32_t addr)
{
    if (boot_psx_game_address_in_text(addr)) return boot_psx_dispatch_game_compiled(cpu, addr);
    for (int i = 0; i < WII_MAIN_PROGS; i++) {
        int k = (s_cur + i) % WII_MAIN_PROGS;
        use(k);
        if (s_prog[k].dispatch(cpu, addr)) return 1;
    }
    return 0;
}

#define COMBINE_CHECK(name, field) \
    int name(uint32_t addr) \
    { \
        if (boot_psx_game_address_in_text(addr)) return boot_##name(addr); \
        for (int i = 0; i < WII_MAIN_PROGS; i++) { \
            int k = (s_cur + i) % WII_MAIN_PROGS; \
            use(k); \
            if (s_prog[k].field(addr)) return 1; \
        } \
        return 0; \
    }
COMBINE_CHECK(psx_game_text_native_ok, ok)
COMBINE_CHECK(psx_game_text_native_ok_full, ok_full)

#define COMBINE_ANY(name, field) \
    int name(uint32_t addr) \
    { \
        if (boot_##name(addr)) return 1; \
        for (int k = 0; k < WII_MAIN_PROGS; k++) \
            if (s_prog[k].field(addr)) return 1; \
        return 0; \
    }
COMBINE_ANY(psx_game_address_in_text, in_text)
COMBINE_ANY(psx_game_is_function_entry, is_entry)
