#!/usr/bin/env python3
"""Shrink the PSXRecomp runtime's debug/trace buffers so it fits the Wii's
24 MB of main memory. Idempotent: safe to run on an already patched tree.

usage: patch_runtime.py <psxrecomp checkout>
"""
import re, sys

root = sys.argv[1] + "/runtime"

# (file, regex for the whole #define line's value, replacement text)
PATCHES = [
    ("include/fntrace.h",       r"(#define FNTRACE_RING_CAP\s+)\(1u << 22\)", r"\g<1>(1u << 8)"),
    ("include/sio.h",           r"(#define SIO_IRQ_RING_CAP\s+)\(1 << 20\)",  r"\g<1>(1 << 8)"),
    ("include/sio.h",           r"(#define SIO_TXN_CAP\s+)\(1 << 16\)",       r"\g<1>(1 << 8)"),
    ("include/sio.h",           r"(#define SIO_TRACE_CAP\s+)\(1 << 20\)",      r"\g<1>(1 << 8)"),
    ("src/audio_trace.c",       r"(#define EV_RING_CAP\s+)\(1u << 19\)",       r"\g<1>(1u << 8)"),
    ("src/spu.c",               r"(#define SPU_EVENT_CAP\s+)\(1u << 20\)",      r"\g<1>(1u << 8)"),
    ("src/spu.c",               r"(#define SPU_CD_RING_FRAMES\s+)\(44100u \* 8u\)", r"\g<1>(44100u * 2u)"),
    ("include/bios_hle.h",      r"(#define PSX_HLE_RING_CAP\s+)16384",         r"\g<1>256"),
    ("src/parity_trace.c",      r"(#define PARITY_RING_CAP\s+)131072u",         r"\g<1>256u"),
    ("src/overlay_loader.c",    r"(#define FP_CAP\s+)\(1u << 16\)",            r"\g<1>(1u << 6)"),
    ("src/overlay_loader.c",    r"(#define CACHE_IDX_CAP\s+)4096",             r"\g<1>64"),
    ("src/overlay_loader.c",    r"(#define MAX_LOADED_DLLS\s+)4096",           r"\g<1>8"),
    ("include/overlay_api.h",   r"(#define PSX_OVERLAY_CANDIDATE_CAP\s+)32768", r"\g<1>256"),
    ("include/dirty_ram_interp.h", r"(#define DIRTY_RAM_PC_TABLE_SIZE\s+)262144", r"\g<1>16384"),
    ("include/cdrom.h",         r"(#define CDROM_TRACE_CAP\s+)\(1 << 16\)",     r"\g<1>(1 << 8)"),
    ("include/cdrom.h",         r"(#define CDROM_SECTOR_HISTORY_CAP\s+)\(1 << 13\)", r"\g<1>(1 << 6)"),
    ("include/dma.h",           r"(#define DMA_CDROM_HISTORY_CAP\s+)\(1 << 13\)", r"\g<1>(1 << 6)"),
    ("src/audio_trace.c",       r"(#define PCM_RING_FRAMES\s+)\(1u << 22\)", r"\g<1>(1u << 10)"),
    ("src/device_trace.c",      r"(#define DEVTRACE_RING_CAP\s+)\(1u << 20\)", r"\g<1>(1u << 8)"),
    ("include/event_ring.h",    r"(#define EVENT_RING_CAP\s+)\(1u << 16\)",   r"\g<1>(1u << 8)"),
    ("include/dma.h",           r"(#define DMA_TRACE_CAP\s+)\(1 << 14\)",       r"\g<1>(1 << 8)"),
    ("include/starvation_ring.h", r"(#define STARVATION_RING_CAP\s+)\(1 << 14\)", r"\g<1>(1 << 8)"),
    ("src/latency_ring.c",      r"(#define LAT_RING_SIZE\s+)4096",              r"\g<1>64"),
    ("include/card_data_writes.h", r"(#define CARD_DATA_WRITES_CAP\s+)4096",     r"\g<1>64"),
    ("src/crash_trace.c",       r"static char xp\[1048576\];",                  "static char xp[4096];"),
    ("include/mod_memory.h",    r"(#define PSX_MOD_GPU_DMA_APERTURE_SIZE\s+)\(1u \* 1024u \* 1024u\)", r"\g<1>4096u"),
    ("src/memory.c",            r"(#define MOD_MEMORY_SIZE\s+)\(1u \* 1024u \* 1024u\)", r"\g<1>4096u"),
    ("src/dma.c",               r"(#define CD_DMA_LOG_CAP\s+)65536",           r"\g<1>256"),
    ("include/cdrom.h",         r"(#define CDROM_COMMAND_HISTORY_CAP\s+)\(1 << 13\)", r"\g<1>(1 << 6)"),
    ("src/dirty_ram_interp.c",  r"(#define XDET_CAP\s+)16384u",                r"\g<1>256u"),
    ("src/overlay_loader.c",    r"(#define NRING_CAP\s+)16384",                r"\g<1>256"),
    ("src/interrupts.c",        r"(#define IRQCTX_RING_CAP\s+)4096u",          r"\g<1>256u"),
    ("src/crash_trace.c",       r"static char buf\[8 \* 1024 \* 1024\];",      "static char buf[4096];"),
]

# Byte order: guest RAM is a little-endian image; the Wii is big-endian.
EXACT = [
    ("include/psx_cyc.h",
     "        uint32_t value;\n        memcpy(&value, g_psx_ram + (phys & 0x1FFFFFu), sizeof(value));\n        return value;",
     "        uint32_t value;\n        memcpy(&value, g_psx_ram + (phys & 0x1FFFFFu), sizeof(value));\n"
     "#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__\n        value = __builtin_bswap32(value);\n#endif\n        return value;"),
    ("include/psx_cyc.h",
     "        uint16_t value;\n        memcpy(&value, g_psx_ram + (phys & 0x1FFFFFu), sizeof(value));\n        return value;",
     "        uint16_t value;\n        memcpy(&value, g_psx_ram + (phys & 0x1FFFFFu), sizeof(value));\n"
     "#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__\n        value = __builtin_bswap16(value);\n#endif\n        return value;"),
    ("src/dma.c",
     "            uint32_t got =\n                mdec_dma_write_words((const uint32_t *)(ram + addr), n);",
     "            uint32_t got;\n            {\n                uint32_t tmp[256];\n                uint32_t cn = n > 256u ? 256u : n;\n"
     "                for (uint32_t k = 0; k < cn; k++) {\n                    const uint8_t *q = ram + addr + k * 4u;\n"
     "                    tmp[k] = (uint32_t)q[0] | ((uint32_t)q[1] << 8) | ((uint32_t)q[2] << 16) | ((uint32_t)q[3] << 24);\n                }\n"
     "                got = mdec_dma_write_words(tmp, cn);\n            }"),
]
EXACT.append(("src/dma.c",
     "            uint32_t got = mdec_dma_write_words(\n                (const uint32_t *)(memory_get_ram_ptr() + addr), total_words);",
     "            uint32_t got;\n            {\n                uint32_t tmp[256];\n                uint32_t cn = total_words > 256u ? 256u : total_words;\n"
     "                const uint8_t *rp = memory_get_ram_ptr() + addr;\n"
     "                for (uint32_t k = 0; k < cn; k++)\n"
     "                    tmp[k] = (uint32_t)rp[k*4u] | ((uint32_t)rp[k*4u+1u] << 8) | ((uint32_t)rp[k*4u+2u] << 16) | ((uint32_t)rp[k*4u+3u] << 24);\n"
     "                got = mdec_dma_write_words(tmp, cn);\n            }"))
EXACT.append(("src/memory.c",
     "        uint32_t v;\n        memcpy(&v, &ram[phys], sizeof(v));\n        /* Targeted main-RAM read watch",
     "        uint32_t v;\n        memcpy(&v, &ram[phys], sizeof(v));\n"
     "#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__\n        v = __builtin_bswap32(v);\n#endif\n"
     "        /* Targeted main-RAM read watch"))
EXACT.append(("src/cdrom.c",
     "        memcpy(&val, RB_.data + RB_.pos, 4);\n        RB_.pos += 4;",
     "        memcpy(&val, RB_.data + RB_.pos, 4);\n"
     "#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__\n        val = __builtin_bswap32(val);\n#endif\n"
     "        RB_.pos += 4;"))
# Several main programs load over the same RAM, each with its own reference
# text: let the host switch the active image without resetting the guard.
EXACT.append(("src/memory.c",
     "void dirty_ram_text_exact_mismatch_stats(uint64_t *count,",
     "void dirty_ram_select_text_image(uint32_t phys_lo, const uint8_t *bytes,\n"
     "                                 uint32_t len) {\n"
     "    text_ref_image = (uint8_t *)bytes;\n"
     "    text_ref_lo = phys_lo;\n"
     "    text_ref_hi = phys_lo + len;\n"
     "    memset(text_verified_bitmap, 0, sizeof(text_verified_bitmap));\n"
     "}\n\n"
     "void dirty_ram_text_exact_mismatch_stats(uint64_t *count,"))
# Text guard cache. Once a main program has been loaded its text pages stay
# flagged (the clean baseline is only taken for the boot EXE), so every
# dispatch into it memcmp'd the function's ranges against the reference. A
# 256-byte chunk whose contents compared equal to the active reference is now
# remembered as such until something could change it: a page or executable-
# range mark, a guest write that differs from the reference, or a switch of
# reference image. Chunks rather than pages, because the reference covers the
# whole EXE and the game's data shares pages with its code. This is the same
# trust the guard already places in never-touched pages.
EXACT.append(("src/memory.c",
     "static inline void dirty_ram_mark_page(uint32_t phys) {\n    if (phys >= RAM_SIZE) return;\n"
     "    uint32_t page = phys >> DIRTY_RAM_PAGE_SHIFT;\n    uint32_t bit = 1u << (page & 31u);\n",
     "#define TEXT_CHUNK_SHIFT 8u\n"
     "static uint32_t text_verified_bitmap[(RAM_SIZE >> TEXT_CHUNK_SHIFT) / 32u];\n"
     "static inline void text_unverify_page(uint32_t page) {   /* 16 chunks per 4 KB page */\n"
     "    text_verified_bitmap[page >> 1] &= ~(0xFFFFu << ((page & 1u) * 16u));\n}\n"
     "static inline int text_chunks_verified(uint32_t phys, uint32_t len) {\n"
     "    for (uint32_t c = phys >> TEXT_CHUNK_SHIFT; c <= (phys + len - 1u) >> TEXT_CHUNK_SHIFT; c++)\n"
     "        if (!(text_verified_bitmap[c >> 5] & (1u << (c & 31u)))) return 0;\n"
     "    return 1;\n}\n\n"
     "static inline void dirty_ram_mark_page(uint32_t phys) {\n    if (phys >= RAM_SIZE) return;\n"
     "    uint32_t page = phys >> DIRTY_RAM_PAGE_SHIFT;\n    uint32_t bit = 1u << (page & 31u);\n"
     "    text_unverify_page(page);\n"))
EXACT.append(("src/memory.c",
     "    for (uint32_t page = first_page; page <= last_page; page++) {\n"
     "        dirty_ram_bitmap[page >> 5] |= (1u << (page & 31u));\n    }\n    g_dirty_ram_code_gen++;",
     "    for (uint32_t page = first_page; page <= last_page; page++) {\n"
     "        dirty_ram_bitmap[page >> 5] |= (1u << (page & 31u));\n"
     "        text_unverify_page(page);\n    }\n    g_dirty_ram_code_gen++;"))
EXACT.append(("src/memory.c",
     "        uint32_t page = phys >> DIRTY_RAM_PAGE_SHIFT;\n        text_modified_bitmap[page >> 5] |= (1u << (page & 31u));\n    }\n}",
     "        uint32_t page = phys >> DIRTY_RAM_PAGE_SHIFT;\n        text_modified_bitmap[page >> 5] |= (1u << (page & 31u));\n"
     "        uint32_t c = phys >> TEXT_CHUNK_SHIFT;   /* a write never straddles chunks */\n"
     "        text_verified_bitmap[c >> 5] &= ~(1u << (c & 31u));\n    }\n}"))
EXACT.append(("src/memory.c",
     "    memset(text_modified_bitmap, 0, sizeof(text_modified_bitmap));\n    memset(text_diverged_bitmap, 0, sizeof(text_diverged_bitmap));\n",
     "    memset(text_modified_bitmap, 0, sizeof(text_modified_bitmap));\n    memset(text_diverged_bitmap, 0, sizeof(text_diverged_bitmap));\n"
     "    memset(text_verified_bitmap, 0, sizeof(text_verified_bitmap));\n"))
EXACT.append(("src/memory.c",
     "    if (!(text_modified_bitmap[page >> 5] & bit) && !dirty_ram_is_dirty(phys))\n        return 1;\n",
     "    if (!(text_modified_bitmap[page >> 5] & bit) && !dirty_ram_is_dirty(phys))\n        return 1;\n"
     "    if (text_chunks_verified(phys, text_ref_hi - phys < 256u ? text_ref_hi - phys : 256u))\n        return 1;\n"))
EXACT.append(("src/memory.c",
     "        any = 1;\n        /* Page-clean fast path:",
     "        any = 1;\n        if (text_chunks_verified(phys, len)) continue;\n        /* Page-clean fast path:"))
EXACT.append(("src/memory.c",
     "            g_text_native_blocked++;\n            return 0;\n        }\n    }\n    if (!any) {",
     "            g_text_native_blocked++;\n            return 0;\n        }\n"
     "        /* The range matched: verify the chunks it touches, once. */\n"
     "        for (uint32_t c = phys >> TEXT_CHUNK_SHIFT; c <= (phys + len - 1u) >> TEXT_CHUNK_SHIFT; c++) {\n"
     "            uint32_t lo = c << TEXT_CHUNK_SHIFT, sz = 1u << TEXT_CHUNK_SHIFT;\n"
     "            if ((text_verified_bitmap[c >> 5] & (1u << (c & 31u))) ||\n"
     "                lo < text_ref_lo || lo + sz > text_ref_hi) continue;\n"
     "            if (memcmp(ram + lo, text_ref_image + (lo - text_ref_lo), sz) == 0)\n"
     "                text_verified_bitmap[c >> 5] |= 1u << (c & 31u);\n"
     "        }\n    }\n    if (!any) {"))

# Per-instruction timing helpers: at -Os GCC keeps these as separate calls, so
# psx_cyc_step costs three or four nested calls per guest instruction. Force
# them into their callers; each translation unit keeps one psx_cyc_step.
for fn in ("psx_cyc_charge(uint32_t cycles)", "psx_cyc_base(CPUState* cpu)",
           "psx_cyc_deps(CPUState* cpu, uint32_t reg_mask)", "psx_cyc_lds(CPUState* cpu)"):
    EXACT.append(("include/psx_cyc.h", f"static inline void {fn} {{",
                  f"static inline __attribute__((always_inline)) void {fn} {{"))
# CD deadline: once the current CD interrupt has been presented to INTC
# (latched), presentation can do nothing more until the guest acks it through
# an MMIO write, which recomputes deadlines anyway. Advertising it as "due now"
# made every instruction run a full device pass while an interrupt was pending.
EXACT.append(("src/cdrom.c",
     "    if (cdrom_irq_mask_matches_reason(irq_enable, irq_flag)) {\n        int rem = irq_present_rem_cycles();",
     "    if (cdrom_irq_mask_matches_reason(irq_enable, irq_flag) && !cdrom_intc_request_latched) {\n        int rem = irq_present_rem_cycles();"))
# SPU sample scheduler: while the SPU IRQ is enabled it runs at every 768-cycle
# sample boundary and on every deadline recompute. It fetched the whole SPU
# state (a memset plus two 24-voice loops) to test one control bit, and took
# 64-bit modulos, the bulk of a course's runtime on the Wii. Same results.
EXACT.append(("src/spu.c",
     "void spu_get_global_state(SpuGlobalState* out) {",
     "uint16_t spu_get_ctrl(void) { return spu_regs[reg_index(0x1F801DAAu)]; }\n\n"
     "void spu_get_global_state(SpuGlobalState* out) {"))
EXACT.append(("src/interrupts.c",
     "uint32_t psx_spu_sample_event_cycles_to_next(void) {\n    SpuGlobalState state;\n",
     "extern uint16_t spu_get_ctrl(void);\n"
     "/* x % 768 without a 64-bit division: 2^32 = 256 (mod 768). */\n"
     "static inline uint32_t cyc_mod768(uint64_t x) {\n"
     "    return (((uint32_t)(x >> 32) % 3u) * 256u + (uint32_t)x % 768u) % 768u;\n}\n\n"
     "uint32_t psx_spu_sample_event_cycles_to_next(void) {\n"))
EXACT.append(("src/interrupts.c",
     "    spu_get_global_state(&state);\n    if ((state.ctrl & 0x0040u) == 0) {\n        g_spu_sample_ctrl_rejects++;",
     "    if ((spu_get_ctrl() & 0x0040u) == 0) {\n        g_spu_sample_ctrl_rejects++;"))
EXACT.append(("src/interrupts.c",
     "    const uint32_t phase = (uint32_t)(psx_get_cycle_count() % 768u);",
     "    const uint32_t phase = cyc_mod768(psx_get_cycle_count());"))
EXACT.append(("src/interrupts.c",
     "    SpuGlobalState state;\n    spu_get_global_state(&state);\n    if ((state.ctrl & 0x0040u) != 0) {\n"
     "        g_spu_sample_enabled_services++;\n        g_spu_sample_last_service_phase = (uint32_t)(psx_cycle_count % 768u);\n    }\n"
     "    if ((psx_cycle_count % 768u) == 0) {\n        g_spu_sample_raw_boundaries++;\n"
     "        if ((psx_get_cycle_count() % 768u) != 0)\n            g_spu_sample_deferred_mismatches++;\n    }\n"
     "    if ((state.ctrl & 0x0040u) != 0 &&\n        (psx_get_cycle_count() % 768u) == 0) {",
     "    const uint16_t spu_ctrl = spu_get_ctrl();\n    const uint32_t phase = cyc_mod768(psx_get_cycle_count());\n"
     "    if ((spu_ctrl & 0x0040u) != 0) {\n"
     "        g_spu_sample_enabled_services++;\n        g_spu_sample_last_service_phase = cyc_mod768(psx_cycle_count);\n    }\n"
     "    if (cyc_mod768(psx_cycle_count) == 0) {\n        g_spu_sample_raw_boundaries++;\n"
     "        if (phase != 0)\n            g_spu_sample_deferred_mismatches++;\n    }\n"
     "    if ((spu_ctrl & 0x0040u) != 0 && phase == 0) {"))

# SPU reverb work-area addresses: every term is below 2^20, so the int64
# modulo (a libgcc call on the Wii, several per reverb step) can be 32-bit.
EXACT.append(("src/spu.c",
     "    int64_t  rel  = (int64_t)rev_cur - (int64_t)base\n"
     "                  + ((int64_t)RREG(reg_addr) << 3) + (int64_t)bias;\n"
     "    rel %= (int64_t)size;\n    if (rel < 0) rel += (int64_t)size;",
     "    int32_t  rel  = (int32_t)rev_cur - (int32_t)base\n"
     "                  + ((int32_t)RREG(reg_addr) << 3) + bias;\n"
     "    rel %= (int32_t)size;\n    if (rel < 0) rel += (int32_t)size;"))

# Opt-in fast GPU linked-list DMA (g_dma_gpu_ll_fast, off by default). The
# faithful model takes a device event per packet header and payload, a full
# device pass each, thousands per frame for a scene of line primitives. Fast
# mode predicts the completion when the list starts, advertises only that,
# and walks the packets lazily up to the current cycle whenever the DMA is
# advanced, so register and GPU reads still see the same progress. The cost:
# a packet the CPU rewrites after the DMA started is read when the walk gets
# there rather than at its own cycle, and a list the CPU shortens completes
# late. If a list outlives its prediction it is predicted again.
EXACT.append(("include/dma_gpu_ll.h",
     "    uint32_t empty_rank;\n} DMAGPULinkedList;",
     "    uint32_t empty_rank;\n    uint32_t fast_left;      /* fast mode: predicted cycles to completion */\n} DMAGPULinkedList;"))
EXACT.append(("include/dma_gpu_ll.h",
     "void dma_gpu_ll_cancel(DMAGPULinkedList *state);\n",
     "void dma_gpu_ll_cancel(DMAGPULinkedList *state);\n"
     "extern int g_dma_gpu_ll_fast;\n"
     "void dma_gpu_ll_predict(DMAGPULinkedList *state, const DMAGPULinkedListOps *ops, void *opaque);\n"))
EXACT.append(("src/dma_gpu_ll.c",
     "uint32_t dma_gpu_ll_cycles_to_event(const DMAGPULinkedList *state) {\n    if (!state->active) return UINT32_MAX;\n",
     "int g_dma_gpu_ll_fast;\n\n"
     "void dma_gpu_ll_predict(DMAGPULinkedList *s, const DMAGPULinkedListOps *ops, void *opaque) {\n"
     "    s->fast_left = 0;\n"
     "    if (!g_dma_gpu_ll_fast || !s->active || !ops || !ops->read_word) return;\n"
     "    uint64_t total = 0;\n"
     "    uint32_t phase = s->phase, cur = s->current_addr, next = s->next_addr;\n"
     "    uint32_t wc = s->word_count, n = s->nodes_processed, rem = s->cycles_remaining;\n"
     "    for (uint32_t guard = 0; guard < 0x80000u; guard++) {\n"
     "        total += rem ? rem : 1u;\n"
     "        if (phase == DMA_GPU_LL_PHASE_HEADER) {\n"
     "            if (n >= s->max_nodes) break;\n"
     "            cur = resolve_address(ops, opaque, cur);\n"
     "            uint32_t header = ops->read_word(opaque, cur);\n"
     "            wc = header >> 24; next = header & 0x00FFFFFFu; n++;\n"
     "            if (wc != 0) { phase = DMA_GPU_LL_PHASE_PAYLOAD; rem = DMA_GPU_LL_SETUP_CYCLES; continue; }\n"
     "            if (next == 0x00FFFFFFu) break;\n"
     "            cur = resolve_address(ops, opaque, next); rem = DMA_GPU_LL_HEADER_CYCLES; continue;\n"
     "        }\n"
     "        if (phase == DMA_GPU_LL_PHASE_PAYLOAD) {\n"
     "            if (next == 0x00FFFFFFu) { phase = DMA_GPU_LL_PHASE_COMPLETE; rem = wc; continue; }\n"
     "            phase = DMA_GPU_LL_PHASE_HEADER; cur = resolve_address(ops, opaque, next);\n"
     "            rem = wc + DMA_GPU_LL_HEADER_CYCLES; continue;\n"
     "        }\n"
     "        break;\n"
     "    }\n"
     "    s->fast_left = total > 0x7FFFFFFFu ? 0x7FFFFFFFu : (uint32_t)total;\n"
     "}\n\n"
     "uint32_t dma_gpu_ll_cycles_to_event(const DMAGPULinkedList *state) {\n    if (!state->active) return UINT32_MAX;\n"
     "    if (state->fast_left) return state->fast_left;\n"))
EXACT.append(("src/dma_gpu_ll.c",
     "void dma_gpu_ll_advance(DMAGPULinkedList *state, uint32_t cycles,\n                        const DMAGPULinkedListOps *ops, void *opaque) {\n"
     "    if (!state->active || cycles == 0 || !ops || !ops->read_word) return;",
     "static void ll_advance_one(DMAGPULinkedList *state, uint32_t cycles,\n"
     "                           const DMAGPULinkedListOps *ops, void *opaque);\n\n"
     "void dma_gpu_ll_advance(DMAGPULinkedList *state, uint32_t cycles,\n                        const DMAGPULinkedListOps *ops, void *opaque) {\n"
     "    if (!state->fast_left) { ll_advance_one(state, cycles, ops, opaque); return; }\n"
     "    if (!state->active || cycles == 0 || !ops || !ops->read_word) return;\n"
     "    state->fast_left = state->fast_left > cycles ? state->fast_left - cycles : 0u;\n"
     "    while (state->active && cycles >= (state->cycles_remaining ? state->cycles_remaining : 1u)) {\n"
     "        uint32_t step = state->cycles_remaining ? state->cycles_remaining : 1u;\n"
     "        cycles -= step;\n"
     "        ll_advance_one(state, step, ops, opaque);\n"
     "    }\n"
     "    if (state->active && cycles) ll_advance_one(state, cycles, ops, opaque);\n"
     "    if (state->active && !state->fast_left) dma_gpu_ll_predict(state, ops, opaque);\n"
     "}\n\n"
     "static void ll_advance_one(DMAGPULinkedList *state, uint32_t cycles,\n"
     "                           const DMAGPULinkedListOps *ops, void *opaque) {\n"
     "    if (!state->active || cycles == 0 || !ops || !ops->read_word) return;"))
EXACT.append(("src/dma.c",
     "    dma_gpu_ll_start(&gpu_linked_list, start_addr, 0x40000u);\n",
     "    dma_gpu_ll_start(&gpu_linked_list, start_addr, 0x40000u);\n"
     "    dma_gpu_ll_predict(&gpu_linked_list, &gpu_ll_ops, NULL);\n"))

for rel, old, new in EXACT:
    path = f"{root}/{rel}"
    text = open(path).read()
    if old in text and new not in text:
        open(path, "w").write(text.replace(old, new, 1))
        print(f"patch_runtime: patched {rel}")

changed = 0
for rel, pat, rep in PATCHES:
    path = f"{root}/{rel}"
    text = open(path).read()
    new, n = re.subn(pat, rep, text)
    if not n and not re.search(re.sub(r"\\\(.*", "", pat.split(")")[0] + ")") if False else "", text):
        pass
    if n:
        open(path, "w").write(new)
        changed += n
print(f"patch_runtime: {changed} edits applied")
