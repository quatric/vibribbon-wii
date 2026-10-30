#!/usr/bin/env python3
"""Post-process the recompiler's dispatch tables (build/gen/<program>/*_dispatch.c).

The generated code checks its static-code guard on every transfer between
functions, and each check binary-searches the program's dispatch table. The
table is constant, so a small direct-mapped memo in front of the search
returns the same entry without the search.

It also adds psx_dispatch_game_compiled_checked, the dispatch without its
static-code check, for a caller that has just passed that check for the same
address, and psx_game_code_ranges_each, which reports the emitted code ranges
(source/wii_dispatch.c).

In the BIOS's dispatcher, every call and return of game code tests whether a
target in the BIOS shell's RAM window is game text by running the native-code
guard; the test only matters while the call is unhandled and the game-start
route has not already taken it, so it is made to run only then.
"""
import pathlib
import sys

OLD = """static const PsxGameDispatchEntry* psx_game_find_entry(uint32_t addr) {
    const uint32_t want = addr & 0x1FFFFFFFu;
    uint32_t lo = 0, hi = PSX_GAME_DISPATCH_COUNT;"""

NEW = """static const PsxGameDispatchEntry* psx_game_search_entry(uint32_t want);

/* Direct-mapped memo in front of the table search (scripts/patch_gen.py). */
static const PsxGameDispatchEntry* psx_game_find_entry(uint32_t addr) {
    static uint32_t s_key[512];
    static const PsxGameDispatchEntry* s_val[512];
    const uint32_t want = addr & 0x1FFFFFFFu;
    const uint32_t h = (want >> 2) & 511u;
    if (s_key[h] != want + 1u) {
        s_val[h] = psx_game_search_entry(want);
        s_key[h] = want + 1u;
    }
    return s_val[h];
}

static const PsxGameDispatchEntry* psx_game_search_entry(uint32_t want) {
    uint32_t lo = 0, hi = PSX_GAME_DISPATCH_COUNT;"""

CHECKED_AT = "/* 1 iff addr is a re-enterable compiled entry/continuation (no exec). */"
RANGES = """/* Report every emitted code range (scripts/patch_gen.py). */
void psx_game_code_ranges_each(void (*fn)(uint32_t lo, uint32_t len)) {
    for (uint32_t i = 0; i < sizeof(k_psx_game_code_ranges) / sizeof(k_psx_game_code_ranges[0]); i++)
        fn(k_psx_game_code_ranges[i].lo, k_psx_game_code_ranges[i].len);
}

"""
CHECKED = """/* psx_dispatch_game_compiled for an address whose static-code check the
 * caller has just passed (scripts/patch_gen.py). */
int psx_dispatch_game_compiled_checked(CPUState* cpu, uint32_t addr) {
    const PsxGameDispatchEntry* entry = psx_game_find_entry(addr);
    psx_check_interrupts_dispatch_entry(cpu, addr);
    cpu->pc = entry->resume_pc;
    {
        extern int psx_vsync_query_hle_try(CPUState* cpu, uint32_t addr);
        if (psx_vsync_query_hle_try(cpu, addr)) return 1;
    }
    entry->fn(cpu);
    return 1;
}

"""
DISPATCH = """    if (entry->range_count == 0 || !dirty_ram_text_native_ok_ranges_from(
            &k_psx_game_code_ranges[entry->range_index].lo, entry->range_count, addr)) return 0;
    psx_check_interrupts_dispatch_entry(cpu, addr);
    cpu->pc = entry->resume_pc;
    /* load_accel: cycle-faithful VSync(-1) query (config-gated). */
    {
        extern int psx_vsync_query_hle_try(CPUState* cpu, uint32_t addr);
        if (psx_vsync_query_hle_try(cpu, addr)) return 1;
    }
    entry->fn(cpu);
    return 1;
}"""

SHELL_OLD = """        int game_text_in_shell_window =
            game_phys >= 0x00030000u && game_phys <= 0x0005AFFFu &&"""
SHELL_NEW = """        int game_text_in_shell_window = !found && !game_shell_overlap &&
            game_phys >= 0x00030000u && game_phys <= 0x0005AFFFu &&"""

for path in sys.argv[1:]:
    p = pathlib.Path(path)
    text = p.read_text()
    if "psx_dispatch_impl" in text and "PSX_GAME_DISPATCH_COUNT" not in text:   # the BIOS's
        if SHELL_OLD in text:
            text = text.replace(SHELL_OLD, SHELL_NEW)
        elif SHELL_NEW not in text:
            sys.exit(f"patch_gen: {p.name}: shell-window test not found")
        p.write_text(text)
        print(f"patch_gen: patched {p.name}")
        continue
    if OLD in text:
        text = text.replace(OLD, NEW)
    elif "psx_game_search_entry" not in text:
        sys.exit(f"patch_gen: {p.name}: dispatch lookup not found")
    if CHECKED_AT not in text:
        sys.exit(f"patch_gen: {p.name}: dispatch body not found")
    if "psx_dispatch_game_compiled_checked" not in text:
        if DISPATCH not in text:
            sys.exit(f"patch_gen: {p.name}: dispatch body not found")
        text = text.replace(CHECKED_AT, CHECKED + CHECKED_AT, 1)
    if "psx_game_code_ranges_each" not in text:
        text = text.replace(CHECKED_AT, RANGES + CHECKED_AT, 1)
    p.write_text(text)
    print(f"patch_gen: patched {p.name}")
