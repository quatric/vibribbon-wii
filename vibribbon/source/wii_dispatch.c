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
    extern int p##_psx_dispatch_game_compiled_checked(CPUState *cpu, uint32_t addr); \
    extern int p##_psx_game_address_in_text(uint32_t addr); \
    extern int p##_psx_game_is_function_entry(uint32_t addr); \
    extern int p##_psx_game_text_native_ok(uint32_t addr); \
    extern int p##_psx_game_text_native_ok_full(uint32_t addr); \
    extern void p##_psx_game_code_ranges_each(void (*fn)(uint32_t lo, uint32_t len));
PROG_DECLS(boot)
PROG_DECLS(main)
PROG_DECLS(title)

typedef struct {
    int (*dispatch)(CPUState *cpu, uint32_t addr);
    int (*dispatch_checked)(CPUState *cpu, uint32_t addr);
    int (*in_text)(uint32_t addr);
    int (*is_entry)(uint32_t addr);
    int (*ok)(uint32_t addr);
    int (*ok_full)(uint32_t addr);
    const uint8_t *img;
    uint32_t lo, len;
} MainProg;

#define PROG(p) { p##_psx_dispatch_game_compiled, p##_psx_dispatch_game_compiled_checked, \
                  p##_psx_game_address_in_text, \
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

/* Selecting an image drops the guard's verified chunks, so an address that no
 * program accepts (code the game patched runs interpreted) must not flip the
 * image back and forth on every check. After such a miss the active program
 * stays, and the others are tried again only once code in RAM changes. */
extern uint32_t g_dirty_ram_code_gen;
static uint32_t s_settled_gen;

enum { OP_DISPATCH, OP_OK, OP_OK_FULL };

static inline int call(int k, int op, CPUState *cpu, uint32_t addr)
{
    switch (op) {
    case OP_DISPATCH: return s_prog[k].dispatch(cpu, addr);
    case OP_OK:       return s_prog[k].ok(addr);
    default:          return s_prog[k].ok_full(addr);
    }
}

static inline int try_progs(int op, CPUState *cpu, uint32_t addr)
{
    int cur = s_cur;
    if (call(cur, op, cpu, addr)) return 1;
    if (s_settled_gen == g_dirty_ram_code_gen) return 0;
    for (int i = 1; i < WII_MAIN_PROGS; i++) {
        int k = (cur + i) % WII_MAIN_PROGS;
        use(k);
        if (call(k, op, cpu, addr)) return 1;
    }
    use(cur);
    s_settled_gen = g_dirty_ram_code_gen;
    return 0;
}

/* Every guest call and return dispatches through here: the runtime checks
 * that the target's static code is intact (psx_game_text_native_ok), then
 * dispatches it, which checks again. The guard's answer for an address only
 * changes when its change counter does (any write into game text, or another
 * reference image), so a passed check is remembered per address, and a
 * dispatch right after a passed check skips the second one. */
extern uint32_t dirty_ram_text_seq(void);
extern void dirty_ram_text_note_code_range(uint32_t lo, uint32_t len);
extern void dirty_ram_text_code_ranges_done(void);

/* Tell the guard which pages hold compiled code, so writes to the programs'
 * data do not count as changes. */
static void note_code_pages(void)
{
    static int done;
    if (done) return;
    done = 1;
    boot_psx_game_code_ranges_each(dirty_ram_text_note_code_range);
    main_psx_game_code_ranges_each(dirty_ram_text_note_code_range);
    title_psx_game_code_ranges_each(dirty_ram_text_note_code_range);
    dirty_ram_text_code_ranges_done();
}
typedef struct { uint32_t addr, seq; int prog1; } OkMemo;   /* prog1: program + 1, 0 if empty */
static OkMemo s_ok_memo[4096];   /* a menu frame checks ~1000 addresses */
static OkMemo s_ok_last;

/* The boot loader's functions all lie in its own text, below the main
 * programs, so an address is either the boot loader's or a main program's:
 * one range test saves a table search on every check. */
int psx_dispatch_game_compiled(CPUState *cpu, uint32_t addr)
{
    if (boot_psx_game_address_in_text(addr)) return boot_psx_dispatch_game_compiled(cpu, addr);
    if (addr == s_ok_last.addr && s_cur + 1 == s_ok_last.prog1 && dirty_ram_text_seq() == s_ok_last.seq)
        return s_prog[s_cur].dispatch_checked(cpu, addr);
    return try_progs(OP_DISPATCH, cpu, addr);
}

int psx_game_text_native_ok(uint32_t addr)
{
    note_code_pages();
    if (boot_psx_game_address_in_text(addr)) return boot_psx_game_text_native_ok(addr);
    OkMemo *m = &s_ok_memo[(addr >> 2) & 4095u];
    if (m->addr == addr && m->prog1 == s_cur + 1 && m->seq == dirty_ram_text_seq()) {
        s_ok_last = *m;
        return 1;
    }
    if (!try_progs(OP_OK, 0, addr)) return 0;
    m->addr = addr;
    m->seq = dirty_ram_text_seq();
    m->prog1 = s_cur + 1;
    s_ok_last = *m;
    return 1;
}

int psx_game_text_native_ok_full(uint32_t addr)
{
    if (boot_psx_game_address_in_text(addr)) return boot_psx_game_text_native_ok_full(addr);
    return try_progs(OP_OK_FULL, 0, addr);
}

#define COMBINE_ANY(name, field) \
    int name(uint32_t addr) \
    { \
        if (boot_##name(addr)) return 1; \
        for (int k = 0; k < WII_MAIN_PROGS; k++) \
            if (s_prog[k].field(addr)) return 1; \
        return 0; \
    }
COMBINE_ANY(psx_game_is_function_entry, is_entry)

/* The text ranges are constant, so the answer per address is memoized. */
static int in_text_uncached(uint32_t addr)
{
    if (boot_psx_game_address_in_text(addr)) return 1;
    for (int k = 0; k < WII_MAIN_PROGS; k++)
        if (s_prog[k].in_text(addr)) return 1;
    return 0;
}

int psx_game_address_in_text(uint32_t addr)
{
    static uint32_t key[256];
    static uint8_t val[256], set[256];
    uint32_t h = (addr >> 2) & 255u;
    if (!set[h] || key[h] != addr) {
        val[h] = (uint8_t)in_text_uncached(addr);
        key[h] = addr;
        set[h] = 1;
    }
    return val[h];
}
