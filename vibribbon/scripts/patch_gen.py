#!/usr/bin/env python3
"""Post-process the recompiler's dispatch tables (build/gen/<program>/*_dispatch.c).

The generated code checks its static-code guard on every transfer between
functions, and each check binary-searches the program's dispatch table. The
table is constant, so a small direct-mapped memo in front of the search
returns the same entry without the search.
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

for path in sys.argv[1:]:
    p = pathlib.Path(path)
    text = p.read_text()
    if OLD in text:
        p.write_text(text.replace(OLD, NEW))
        print(f"patch_gen: patched {p.name}")
    elif "psx_game_search_entry" not in text:
        sys.exit(f"patch_gen: {p.name}: dispatch lookup not found")
