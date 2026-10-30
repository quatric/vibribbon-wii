#!/usr/bin/env python3
"""Shrink the PSXRecomp runtime's debug/trace buffers so it fits the Wii's
24 MB of main memory. Idempotent: safe to run on an already patched tree.

usage: patch_runtime.py <psxrecomp checkout>
"""
import os, re, subprocess, sys

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

# Interrupt check at a block edge: when nothing can be delivered (no unmasked
# request, or one the CPU is masking), do psx_check_interrupts' own no-delivery
# bookkeeping without entering it. Its prologue alone was most of the cost of
# every edge. Edges that need anything else still take the full path.
EXACT.append(("src/interrupts.c",
     "void psx_check_interrupts_at(CPUState* cpu, uint32_t resume_pc) {\n"
     "    uint32_t prev = s_compiled_interrupt_resume_pc;\n",
     "void psx_check_interrupts_at(CPUState* cpu, uint32_t resume_pc) {\n"
     "    extern int savestate_pending(void);\n"
     "    extern int psx_netplay_active(void);\n"
     "    extern int g_ls_suppress_record;\n"
     "    if (!in_exception && !s_defer_switch_pending && g_idle_skip_enabled >= 0 &&\n"
     "        ((s_fast_maintenance + 1u) & 0x3FFFu) != 0 &&\n"
     "        !savestate_pending() && !psx_netplay_active()) {\n"
     "        psx_cyc_batch_flush();\n"
     "        uint32_t sr = cpu->cop0[COP0_SR];\n"
     "        int lean = 0;\n"
     "        if ((i_stat & i_mask) != 0) {\n"
     "            if (!(sr & 0x01u) || !(sr & (1u << 10))) {\n"
     "                s_irq_path_entry++;\n"
     "                s_irq_path_fast_sr++;\n"
     "                lean = 1;\n"
     "            }\n"
     "        } else if ((cpu->cop0[COP0_CAUSE] & sr & 0x0300u) == 0) {\n"
     "            s_irq_path_entry++;\n"
     "            if (g_idle_skip_enabled > 0) {\n"
     "                uint32_t check_pc = g_dirty_safe_resume_pc ? g_dirty_safe_resume_pc : resume_pc;\n"
     "                s_last_interrupt_check_pc = check_pc;\n"
     "                s_last_interrupt_check_cycle = psx_get_cycle_count();\n"
     "                uint32_t prev_pc = s_compiled_interrupt_resume_pc;\n"
     "                s_compiled_interrupt_resume_pc = resume_pc;\n"
     "                psx_idle_note_check(cpu, check_pc);\n"
     "                s_compiled_interrupt_resume_pc = prev_pc;\n"
     "            }\n"
     "            if ((i_stat & i_mask) == 0) {\n"
     "                s_irq_path_fast_none++;\n"
     "                lean = 1;\n"
     "            } else {\n"
     "                s_irq_path_entry--;     /* the full path counts this edge */\n"
     "            }\n"
     "        }\n"
     "        if (lean) {\n"
     "            ++s_fast_maintenance;\n"
     "            if (g_ls_suppress_record > 0) g_ls_suppress_record--;\n"
     "            return;\n"
     "        }\n"
     "    }\n"
     "    uint32_t prev = s_compiled_interrupt_resume_pc;\n"))

# Per-range memo for the native-code guard. A range whose 256-byte chunks
# never verify (code next to data the game writes, or code the game patched)
# used to be compared byte by byte on every dispatch. Its result now holds until
# a write lands in one of its pages: every place that drops verified chunks
# also bumps that page's generation, and a write that restores reference bytes
# into an unverified chunk does too, so a range can come back to native.
EXACT.append(("src/memory.c",
     "static inline void text_unverify_page(uint32_t page) {   /* 16 chunks per 4 KB page */\n",
     "static uint32_t text_page_gen[RAM_SIZE >> 12];\n"
     "static uint32_t text_epoch = 1;\n"
     "static inline void text_unverify_page(uint32_t page) {   /* 16 chunks per 4 KB page */\n"
     "    text_page_gen[page]++;\n"))
EXACT.append(("src/memory.c",
     "        uint32_t c = phys >> TEXT_CHUNK_SHIFT;   /* a write never straddles chunks */\n"
     "        text_verified_bitmap[c >> 5] &= ~(1u << (c & 31u));\n"
     "    }\n",
     "        uint32_t c = phys >> TEXT_CHUNK_SHIFT;   /* a write never straddles chunks */\n"
     "        text_verified_bitmap[c >> 5] &= ~(1u << (c & 31u));\n"
     "        text_page_gen[page]++;\n"
     "    } else {\n"
     "        uint32_t c = phys >> TEXT_CHUNK_SHIFT;\n"
     "        if (!(text_verified_bitmap[c >> 5] & (1u << (c & 31u)))) {\n"
     "            text_page_gen[phys >> DIRTY_RAM_PAGE_SHIFT]++;\n"
     "        }\n"
     "    }\n"))
EXACT.append(("src/memory.c",
     "    memset(text_diverged_bitmap, 0, sizeof(text_diverged_bitmap));\n"
     "    memset(text_verified_bitmap, 0, sizeof(text_verified_bitmap));\n",
     "    memset(text_diverged_bitmap, 0, sizeof(text_diverged_bitmap));\n"
     "    memset(text_verified_bitmap, 0, sizeof(text_verified_bitmap));\n"
     "    text_epoch++;\n"))
EXACT.append(("src/memory.c",
     "    text_ref_hi = phys_lo + len;\n"
     "    memset(text_verified_bitmap, 0, sizeof(text_verified_bitmap));\n"
     "}\n",
     "    text_ref_hi = phys_lo + len;\n"
     "    memset(text_verified_bitmap, 0, sizeof(text_verified_bitmap));\n"
     "    text_epoch++;\n"
     "}\n"))
EXACT.append(("src/memory.c",
     "int dirty_ram_text_native_ok_ranges_from(const uint32_t *lo_len_pairs,\n"
     "                                         uint32_t count,\n"
     "                                         uint32_t exec_pc) {\n"
     "    if (!text_ref_image || !lo_len_pairs || count == 0) return 0;\n",
     "static int text_ranges_ok_uncached(const uint32_t *lo_len_pairs,\n"
     "                                   uint32_t count,\n"
     "                                   uint32_t exec_pc);\n"
     "typedef struct { const uint32_t *pairs; uint32_t count, epoch, p0, p1, g0, g1; int ok; } TextRangeMemo;\n"
     "static TextRangeMemo s_range_memo[512];\n"
     "int dirty_ram_text_native_ok_ranges_from(const uint32_t *lo_len_pairs,\n"
     "                                         uint32_t count,\n"
     "                                         uint32_t exec_pc) {\n"
     "    if (!text_ref_image || !lo_len_pairs || count == 0) return 0;\n"
     "    uint32_t at = exec_pc & 0x1FFFFFFFu;\n"
     "    if (aot_blocked(at ? at : (lo_len_pairs[0] & 0x1FFFFFFFu))) {\n"
     "        g_text_native_blocked++;\n"
     "        return 0;\n"
     "    }\n"
     "    TextRangeMemo *m = &s_range_memo[((uintptr_t)lo_len_pairs >> 3) & 511u];\n"
     "    if (m->pairs == lo_len_pairs && m->count == count && m->epoch == text_epoch &&\n"
     "        text_page_gen[m->p0] == m->g0 && text_page_gen[m->p1] == m->g1) {\n"
     "        if (!m->ok) g_text_native_blocked++;\n"
     "        return m->ok;\n"
     "    }\n"
     "    int ok = text_ranges_ok_uncached(lo_len_pairs, count, exec_pc);\n"
     "    /* Memoize ranges within two adjacent pages of RAM. */\n"
     "    uint32_t p0 = UINT32_MAX, p1 = 0;\n"
     "    for (uint32_t i = 0; i < count; i++) {\n"
     "        uint32_t phys = lo_len_pairs[i * 2u] & 0x1FFFFFFFu, len = lo_len_pairs[i * 2u + 1u];\n"
     "        if (len == 0 || phys >= RAM_SIZE || len > RAM_SIZE - phys) { p0 = UINT32_MAX; break; }\n"
     "        if ((phys >> 12) < p0) p0 = phys >> 12;\n"
     "        if (((phys + len - 1u) >> 12) > p1) p1 = (phys + len - 1u) >> 12;\n"
     "    }\n"
     "    if (p0 != UINT32_MAX && p1 - p0 <= 1u) {\n"
     "        m->pairs = lo_len_pairs; m->count = count; m->epoch = text_epoch;\n"
     "        m->p0 = p0; m->p1 = p1; m->g0 = text_page_gen[p0]; m->g1 = text_page_gen[p1];\n"
     "        m->ok = ok;\n"
     "    }\n"
     "    return ok;\n"
     "}\n\n"
     "static int text_ranges_ok_uncached(const uint32_t *lo_len_pairs,\n"
     "                                   uint32_t count,\n"
     "                                   uint32_t exec_pc) {\n"
     "    if (!text_ref_image || !lo_len_pairs || count == 0) return 0;\n"))

# SPU IRQ deadline. While the SPU IRQ is enabled every 44.1 kHz sample was a
# device event (a full device pass and a one-sample render each, ~880 a frame),
# so that an IRQ-address hit is raised on its own sample. Most samples can't
# hit: the only SPU RAM accesses a sample makes are its capture-buffer stores,
# the reverb engine's work-area accesses and the voices' ADPCM block fetches,
# all predictable from the current state. spu_irq_samples_lower_bound() gives
# the first sample that could hit, and a host that renders on the same
# timeline (psx_set_spu_sample_due) turns it into the event instead. The host
# renders the samples in between, as the per-sample pumps would have, before
# any SPU register or DMA access (g_spu_access_hook) and at its own pumps.
EXACT.append(("src/spu.c",
     "static void spu_irq_check(uint32_t addr, uint32_t len) {\n",
     "void (*g_spu_access_hook)(void);\n"
     "extern uint64_t psx_next_service_cycle, g_psx_cycle_fast_limit;\n"
     "extern int psx_in_device_service;\n"
     "extern uint32_t psx_spu_sample_event_cycles_to_next(void);\n"
     "static int spu_bound_valid;            /* spu_irq_samples_lower_bound cache */\n"
     "static void spu_deadline_after_write(void) {\n"
     "    if (psx_in_device_service || psx_next_service_cycle == 0) return;  /* recomputed anyway */\n"
     "    uint32_t d = psx_spu_sample_event_cycles_to_next();\n"
     "    if (d == UINT32_MAX) return;\n"
     "    uint64_t due = psx_get_cycle_count() + d;\n"
     "    if (due < psx_next_service_cycle) { psx_next_service_cycle = due; g_psx_cycle_fast_limit = 0; }\n"
     "}\n\n"
     "static void spu_irq_check(uint32_t addr, uint32_t len) {\n"))
# A write can bring the IRQ closer (new address, key-on, enable) than the
# deadline taken at the MMIO sync: once it is done, pull the deadline in to
# the SPU's. Nothing else moves, so devices are still serviced where they were.
for sig in ("uint32_t spu_read(uint32_t addr) {\n", "uint32_t spu_dma_read(void) {\n"):
    EXACT.append(("src/spu.c", sig, sig + "    if (g_spu_access_hook) g_spu_access_hook();\n"))
for ret, name, params, args in (("void", "spu_write", "uint32_t addr, uint32_t value", "addr, value"),
                                ("void", "spu_dma_write", "uint32_t word", "word")):
    sig = f"{ret} {name}({params}) {{\n"
    EXACT.append(("src/spu.c", sig,
         f"static {ret} {name}_body({params});\n"
         f"{sig}"
         "    if (g_spu_access_hook) g_spu_access_hook();\n"
         f"    {name}_body({args});\n"
         "    spu_bound_valid = 0;\n"
         "    spu_deadline_after_write();\n"
         "}\n"
         f"static {ret} {name}_body({params}) {{\n"))
# The bound depends only on SPU state, so it holds until a render, a write or
# a state load changes that.
for sig in ("void spu_init(void) {\n",
            "int spu_snapshot_read(const uint8_t *p, uint32_t len) {\n",
            "uint8_t*  spu_get_ram_ptr(void){ ",
            "void spu_render(int16_t* out_stereo, int frames) {\n"):
    EXACT.append(("src/spu.c", sig, sig + ("spu_bound_valid = 0; " if sig.endswith(" ")
                                         else "    spu_bound_valid = 0;\n")))
EXACT.append(("src/spu.c",
     "void spu_render(int16_t* out_stereo, int frames) {\n",
     "/* Samples from the next one to render (0) until the first that could raise\n"
     " * the IRQ; UINT32_MAX if none can. A lower bound: a sample it names may\n"
     " * turn out not to hit, but none before it can. */\n"
     "static inline int spu_units_hit(uint32_t addr, uint32_t len, uint32_t target) {\n"
     "    return target >= (addr >> 3) && target <= ((addr + len - 1u) >> 3);\n"
     "}\n"
     "static inline uint32_t spu_samples_until(int32_t need, uint32_t pitch) {\n"
     "    return need <= 0 ? 0u : ((uint32_t)need + pitch - 1u) / pitch;\n"
     "}\n"
     "static uint32_t spu_irq_bound_compute(void) {\n"
     "    if (!(RREG(0x1F801DAAu) & 0x0040u) || irq_flag) return UINT32_MAX;\n"
     "    uint32_t target = RREG(0x1F801DA4u);\n"
     "    uint32_t best = UINT32_MAX;\n"
     "    /* Capture stores: base + capture_pos for bases 0x000..0xC00, 2 bytes on. */\n"
     "    if (target < 0x200u) {\n"
     "        uint32_t unit = target & 0x7Fu;\n"
     "        best = (capture_pos >> 3) == unit ? 0u : ((unit << 3) - capture_pos) % 0x400u / 2u;\n"
     "    }\n"
     "    /* Reverb: one engine step every other sample, inside [mBASE, end). */\n"
     "    if (((target << 3) | 7u) >= rev_mbase()) {\n"
     "        uint32_t k = rev_phase ? 0u : 1u;\n"
     "        if (k < best) best = k;\n"
     "    }\n"
     "    /* Block fetches: a voice fetches when sample_idx * 0x1000 + phase\n"
     "     * reaches 28 * 0x1000, which each fetch takes back off. The next two\n"
     "     * fetch addresses are known; a third is assumed to hit. */\n"
     "    const int32_t blk = SPU_BLOCK_SAMPLES * 0x1000;\n"
     "    for (int i = 0; i < SPU_VOICE_COUNT && best; i++) {\n"
     "        const SpuVoice *v = &voices[i];\n"
     "        if (!v->active) continue;\n"
     "        uint32_t pitch = voice_reg(i, 2) & 0x3FFFu;\n"
     "        int32_t u0 = v->sample_idx * 0x1000 + (int32_t)v->phase;\n"
     "        if (pitch == 0 && u0 < blk) continue;\n"
     "        uint32_t j1 = u0 >= blk ? 0u : spu_samples_until(blk - u0, pitch);\n"
     "        if (j1 >= best) continue;\n"
     "        uint32_t a1 = ((v->flags & 0x01u) ? v->repeat_addr : v->cur_addr) & (SPU_RAM_SIZE - 1u);\n"
     "        if (a1 + 16u > SPU_RAM_SIZE) a1 = 0;\n"
     "        if (spu_units_hit(a1, 16u, target)) { best = j1; continue; }\n"
     "        if (pitch == 0) continue;\n"
     "        uint8_t f1 = spu_ram[a1 + 1u];\n"
     "        uint32_t rep = (f1 & 0x04u) ? a1 : v->repeat_addr;\n"
     "        uint32_t a2 = ((f1 & 0x01u) ? rep : a1 + 16u) & (SPU_RAM_SIZE - 1u);\n"
     "        if (a2 + 16u > SPU_RAM_SIZE) a2 = 0;\n"
     "        uint32_t j2 = spu_samples_until(2 * blk - u0, pitch);\n"
     "        if (j2 <= j1) j2 = j1 + 1u;\n"
     "        if (!spu_units_hit(a2, 16u, target)) {\n"
     "            uint32_t j3 = spu_samples_until(3 * blk - u0, pitch);\n"
     "            j2 = j3 <= j2 ? j2 + 1u : j3;\n"
     "        }\n"
     "        if (j2 < best) best = j2;\n"
     "    }\n"
     "    return best;\n"
     "}\n"
     "uint32_t spu_irq_samples_lower_bound(void) {\n"
     "    static uint32_t bound;\n"
     "    if (!spu_bound_valid) { bound = spu_irq_bound_compute(); spu_bound_valid = 1; }\n"
     "    return bound;\n"
     "}\n\n"
     "void spu_render(int16_t* out_stereo, int frames) {\n"))
# CD input timing. A CD-DA sector lands in the SPU's CD FIFO at whatever
# device-pass boundary follows its due cycle, and a render pops what the FIFO
# holds when it runs, so whether a sample saw the new sector (or recorded an
# underflow) depended on where passes happened to end. Each push now carries
# its due cycle, and a render made for guest time t (g_spu_render_cycle, set by
# a host that renders late) sees only pushes due by t. With a pass ending on
# every sample boundary, as under the per-sample model, nothing changes.
EXACT.append(("src/spu.c",
     "static uint64_t cd_underflow_frames;\n",
     "static uint64_t cd_underflow_frames;\n"
     "uint64_t g_spu_render_cycle = UINT64_MAX;     /* guest time being rendered */\n"
     "uint64_t g_spu_cd_push_cycle = UINT64_MAX;    /* due cycle of the next push */\n"
     "#define CD_PUSH_Q 64u\n"
     "static uint64_t cd_push_due[CD_PUSH_Q];\n"
     "static uint32_t cd_push_end[CD_PUSH_Q];       /* cd_pushed after that push */\n"
     "static uint32_t cd_push_head, cd_push_tail, cd_pushed, cd_popped, cd_visible;\n"))
EXACT.append(("src/spu.c",
     "    cd_overflow_frames = 0;\n"
     "    cd_underflow_frames = 0;\n"
     "}\n",
     "    cd_overflow_frames = 0;\n"
     "    cd_underflow_frames = 0;\n"
     "    cd_push_head = cd_push_tail = cd_pushed = cd_popped = cd_visible = 0;\n"
     "}\n"))
EXACT.append(("src/spu.c",
     "        cd_read_pos = (cd_read_pos + drop) % SPU_CD_RING_FRAMES;\n"
     "        cd_frame_count -= drop;\n",
     "        cd_read_pos = (cd_read_pos + drop) % SPU_CD_RING_FRAMES;\n"
     "        cd_frame_count -= drop;\n"
     "        cd_popped += drop;\n"
     "        if ((int32_t)(cd_popped - cd_visible) > 0) cd_visible = cd_popped;\n"))
EXACT.append(("src/spu.c",
     "    cd_frame_count += in_frames;\n"
     "    cd_push_frames += in_frames;\n",
     "    cd_frame_count += in_frames;\n"
     "    cd_push_frames += in_frames;\n"
     "    cd_pushed += in_frames;\n"
     "    if (cd_push_tail - cd_push_head == CD_PUSH_Q)   /* full: oldest becomes visible */\n"
     "        cd_visible = cd_push_end[cd_push_head++ % CD_PUSH_Q];\n"
     "    cd_push_due[cd_push_tail % CD_PUSH_Q] =\n"
     "        g_spu_cd_push_cycle != UINT64_MAX ? g_spu_cd_push_cycle : psx_get_cycle_count();\n"
     "    cd_push_end[cd_push_tail++ % CD_PUSH_Q] = cd_pushed;\n"))
EXACT.append(("src/spu.c",
     "static int cd_audio_pop(int16_t* left, int16_t* right) {\n"
     "    if (cd_frame_count == 0) return 0;\n",
     "static int cd_audio_pop(int16_t* left, int16_t* right) {\n"
     "    while (cd_push_head != cd_push_tail &&\n"
     "           cd_push_due[cd_push_head % CD_PUSH_Q] <= g_spu_render_cycle)\n"
     "        cd_visible = cd_push_end[cd_push_head++ % CD_PUSH_Q];\n"
     "    if (cd_frame_count == 0 || cd_popped == cd_visible) return 0;\n"
     "    cd_popped++;\n"))
EXACT.append(("src/cdrom.c",
     "        cd_apply_decode_volume(pcm, CDDA_SECTOR_FRAMES);\n"
     "        spu_cd_audio_push(pcm, CDDA_SECTOR_FRAMES);\n",
     "        cd_apply_decode_volume(pcm, CDDA_SECTOR_FRAMES);\n"
     "        {   /* the sector was due cdda_delay (<= 0) cycles ago */\n"
     "            extern uint64_t g_spu_cd_push_cycle;\n"
     "            g_spu_cd_push_cycle = psx_get_cycle_count() + (int64_t)cdda_delay;\n"
     "            spu_cd_audio_push(pcm, CDDA_SECTOR_FRAMES);\n"
     "            g_spu_cd_push_cycle = UINT64_MAX;\n"
     "        }\n"))
# A host that renders a run of samples in one call (each would have been its
# own pump) sets g_spu_render_step to the cycles per sample: every frame then
# sees the CD input due by its own boundary.
EXACT.append(("src/spu.c",
     "static int cd_audio_pop(int16_t* left, int16_t* right) {\n",
     "static int cd_audio_pop(int16_t* left, int16_t* right);\n"
     "uint32_t g_spu_render_step;\n"
     "static inline int cd_audio_pop_frame(int16_t* left, int16_t* right) {\n"
     "    int got = cd_audio_pop(left, right);\n"
     "    g_spu_render_cycle += g_spu_render_step;\n"
     "    return got;\n"
     "}\n\n"
     "static int cd_audio_pop(int16_t* left, int16_t* right) {\n"))
PATCHES.append(("src/spu.c", r"if \(cd_audio_pop\(&cd_l, &cd_r\)\) \{", "if (cd_audio_pop_frame(&cd_l, &cd_r)) {"))
# Fill rectangles: a row that doesn't wrap is filled two pixels per store.
EXACT.append(("src/gpu_sw_renderer.c",
     "    for (int row = 0; row < h; row++) {\n"
     "        int py = (y0 + row) & (VRAM_HEIGHT - 1);\n"
     "        for (int col = 0; col < w; col++) {\n"
     "            int px = (x0 + col) & (VRAM_WIDTH - 1);\n"
     "            g_vram[py * VRAM_WIDTH + px] = color;\n"
     "        }\n"
     "    }\n"
     "    gpu_vram_dirty_mark_rect(x0, y0, w, h);\n",
     "    uint32_t pair = ((uint32_t)color << 16) | color;\n"
     "    for (int row = 0; row < h; row++) {\n"
     "        int py = (y0 + row) & (VRAM_HEIGHT - 1);\n"
     "        if (x0 + w <= VRAM_WIDTH) {\n"
     "            uint16_t *d = &g_vram[py * VRAM_WIDTH + x0], *e = d + w;\n"
     "            if (((uintptr_t)d & 2u) && d < e) *d++ = color;\n"
     "            typedef uint32_t __attribute__((may_alias)) u32a;\n"
     "            for (; d + 2 <= e; d += 2) *(u32a *)d = pair;\n"
     "            if (d < e) *d = color;\n"
     "            continue;\n"
     "        }\n"
     "        for (int col = 0; col < w; col++) {\n"
     "            int px = (x0 + col) & (VRAM_WIDTH - 1);\n"
     "            g_vram[py * VRAM_WIDTH + px] = color;\n"
     "        }\n"
     "    }\n"
     "    gpu_vram_dirty_mark_rect(x0, y0, w, h);\n"))
# The IRQ compare runs on every SPU RAM access the renderer makes: test the
# target against the accessed unit range instead of walking it. Units past the
# top of RAM wrap to 0 as the walk's 16-bit compare did.
EXACT.append(("src/spu.c",
     "    for (uint32_t u = first; u <= last; u++) {\n"
     "        if ((u & 0xFFFFu) == target) {\n"
     "            if (!irq_flag) {\n"
     "                irq_flag = 1;\n"
     "                psx_irq_raise(9u, u << 3);\n"
     "                spu_event_record(SPU_EV_IRQ, 0xFF, u << 3);\n"
     "            }\n"
     "            return;\n"
     "        }\n"
     "    }\n",
     "    uint32_t u;\n"
     "    if (target - first <= last - first) u = target;\n"
     "    else if (last > 0xFFFFu && target + 0x10000u <= last) u = target + 0x10000u;\n"
     "    else return;\n"
     "    if (!irq_flag) {\n"
     "        irq_flag = 1;\n"
     "        psx_irq_raise(9u, u << 3);\n"
     "        spu_event_record(SPU_EV_IRQ, 0xFF, u << 3);\n"
     "    }\n"))
# Reverb work-area accesses all happen inside reverb_step, where the work
# area and the IRQ settings can't change: take them once per step. The IRQ
# compare is skipped when its target lies below the work area (no access can
# reach it), and a power-of-two area wraps with a mask.
EXACT.append(("src/spu.c",
     "static uint32_t rev_ea(uint32_t reg_addr, int32_t bias) {\n"
     "    uint32_t base = rev_mbase();\n"
     "    uint32_t size = SPU_RAM_SIZE - base;\n",
     "static uint32_t rv_base, rv_size;       /* work area for this reverb_step */\n"
     "static int rv_irq;                      /* an access could hit the IRQ address */\n"
     "static uint32_t rev_ea(uint32_t reg_addr, int32_t bias) {\n"
     "    uint32_t base = rv_base;\n"
     "    uint32_t size = rv_size;\n"))
EXACT.append(("src/spu.c",
     "    rel %= (int32_t)size;\n"
     "    if (rel < 0) rel += (int32_t)size;\n",
     "    if ((uint32_t)rel >= size) {\n"
     "        if ((size & (size - 1u)) == 0) {\n"
     "            rel &= (int32_t)(size - 1u);\n"
     "        } else {\n"
     "            rel %= (int32_t)size;\n"
     "            if (rel < 0) rel += (int32_t)size;\n"
     "        }\n"
     "    }\n"))
EXACT.append(("src/spu.c",
     "    uint32_t a = rev_ea(reg_addr, bias);\n"
     "    spu_irq_check(a, 2u);\n",
     "    uint32_t a = rev_ea(reg_addr, bias);\n"
     "    if (rv_irq) spu_irq_check(a, 2u);\n"))
EXACT.append(("src/spu.c",
     "    int16_t  s = clamp16(v);\n"
     "    spu_irq_check(a, 2u);\n",
     "    int16_t  s = clamp16(v);\n"
     "    if (rv_irq) spu_irq_check(a, 2u);\n"))
EXACT.append(("src/spu.c",
     "static void reverb_step(int32_t in_l, int32_t in_r, int wren) {\n",
     "static void reverb_step(int32_t in_l, int32_t in_r, int wren) {\n"
     "    rv_base = rev_mbase();\n"
     "    rv_size = SPU_RAM_SIZE - rv_base;\n"
     "    rv_irq = (RREG(0x1F801DAAu) & 0x0040u) &&\n"
     "             ((((uint32_t)RREG(0x1F801DA4u) << 3) | 7u) >= rv_base);\n"))
# Two debug rings fed on every dispatch (recent targets, stack-domain
# switches); nothing reads them on the Wii.
EXACT.append(("src/fntrace.c",
     "    mod_runtime_on_dispatch(target);\n"
     "    {\n",
     "    mod_runtime_on_dispatch(target);\n"
     "#ifndef PSX_WII\n"
     "    {\n"))
EXACT.append(("src/fntrace.c",
     "            s_spdom_prev_sp = sp_now;\n"
     "        }\n"
     "    }\n",
     "            s_spdom_prev_sp = sp_now;\n"
     "        }\n"
     "    }\n"
     "#endif\n"))
# The card-capture probe is called on every RAM store and almost never armed:
# test its flag inline.
EXACT.append(("src/card_data_writes.c",
     "static volatile int      s_armed = 0;\n",
     "volatile int g_card_data_writes_armed = 0;\n"
     "#define s_armed g_card_data_writes_armed\n"))
EXACT.append(("src/memory.c",
     "extern int card_data_writes_check(uint32_t phys, uint32_t value, uint8_t width);\n",
     "extern int card_data_writes_check(uint32_t phys, uint32_t value, uint8_t width);\n"
     "extern volatile int g_card_data_writes_armed;\n"
     "#define card_data_writes_check(p, v, w) \\\n"
     "    (g_card_data_writes_armed ? card_data_writes_check(p, v, w) : 0)\n"))
# PCM debug taps record every rendered sample; nothing reads them on the Wii.
EXACT.append(("src/audio_trace.c",
     "void audio_trace_pcm(int tap, const int16_t *stereo, int frames)\n{\n",
     "void audio_trace_pcm(int tap, const int16_t *stereo, int frames)\n{\n"
     "#ifdef PSX_WII\n"
     "    (void)tap; (void)stereo; (void)frames;\n"
     "    return;\n"
     "#endif\n"))
# Opt-in (g_psx_mmio_read_slack, 0 = exact): a device-register read may skip
# the catch-up while no device event is due yet and the devices are fewer than
# that many cycles behind. Events and interrupts stay on their cycles; a polled
# counter or status bit can read up to the slack late. Polling loops (the pad
# routine waits on JOY_STAT, timer 1 and I_STAT for every byte) then catch the
# devices up every slack cycles instead of on every iteration.
EXACT.append(("src/psx_cycles.c",
     "/* Exact per-charge path (legacy semantics).",
     "uint32_t g_psx_mmio_read_slack = 0;\n"
     "void psx_devices_mmio_sync_read(void) {\n"
     "    if (g_psx_mmio_read_slack) {\n"
     "        psx_cyc_batch_flush();\n"
     "        if (!s_in_device_service && psx_next_service_cycle != 0 &&\n"
     "            psx_cycle_count < psx_next_service_cycle &&\n"
     "            psx_cycle_count - s_devices_synced_cycle < g_psx_mmio_read_slack)\n"
     "            return;\n"
     "    }\n"
     "    psx_devices_mmio_sync();\n"
     "}\n\n"
     "/* Exact per-charge path (legacy semantics)."))
for w, t in (("32", "uint32_t"), ("16", "uint16_t"), ("8", "uint8_t")):
    EXACT.append(("src/memory.c",
         f"static {t} mmio_read{w}(uint32_t addr) {{\n    psx_devices_mmio_sync();\n",
         f"static {t} mmio_read{w}(uint32_t addr) {{\n"
         "    extern void psx_devices_mmio_sync_read(void);\n"
         "    psx_devices_mmio_sync_read();\n"))
# Every game call checks its code ranges twice (native_ok, then the dispatch
# itself). text_seq counts every change the range memo keys on, so a repeat of
# the last query with nothing changed since is answered before the memo.
EXACT.append(("src/memory.c",
     "static uint32_t text_epoch = 1;\n",
     "static uint32_t text_epoch = 1;\n"
     "static uint32_t text_seq;               /* bumped with any text_page_gen / text_epoch */\n"))
PATCHES.append(("src/memory.c", r"text_page_gen\[([^\]\n]*)\]\+\+;", r"text_page_gen[\1]++; TEXT_SEQ_PAGE(\1);"))
PATCHES.append(("src/memory.c", r"text_epoch\+\+;", r"text_epoch++; text_seq++;"))
EXACT.append(("src/memory.c",
     "    TextRangeMemo *m = &s_range_memo[((uintptr_t)lo_len_pairs >> 3) & 511u];\n"
     "    if (m->pairs == lo_len_pairs && m->count == count && m->epoch == text_epoch &&\n"
     "        text_page_gen[m->p0] == m->g0 && text_page_gen[m->p1] == m->g1) {\n"
     "        if (!m->ok) g_text_native_blocked++;\n"
     "        return m->ok;\n"
     "    }\n",
     "    static const uint32_t *s_fc_pairs;\n"
     "    static uint32_t s_fc_count, s_fc_pc, s_fc_seq;\n"
     "    static int s_fc_ok;\n"
     "    if (lo_len_pairs == s_fc_pairs && count == s_fc_count && exec_pc == s_fc_pc &&\n"
     "        text_seq == s_fc_seq) {\n"
     "        if (!s_fc_ok) g_text_native_blocked++;\n"
     "        return s_fc_ok;\n"
     "    }\n"
     "    TextRangeMemo *m = &s_range_memo[((uintptr_t)lo_len_pairs >> 3) & 511u];\n"
     "    if (m->pairs == lo_len_pairs && m->count == count && m->epoch == text_epoch &&\n"
     "        text_page_gen[m->p0] == m->g0 && text_page_gen[m->p1] == m->g1) {\n"
     "        if (!m->ok) g_text_native_blocked++;\n"
     "        s_fc_pairs = lo_len_pairs; s_fc_count = count; s_fc_pc = exec_pc;\n"
     "        s_fc_seq = text_seq; s_fc_ok = m->ok;\n"
     "        return m->ok;\n"
     "    }\n"))
# The AOT blocklist is empty unless PSX_AOT_BLOCK is set.
EXACT.append(("src/memory.c",
     "static int aot_blocked(uint32_t phys) {\n"
     "    if (s_aot_block_n < 0) aot_block_init();\n",
     "static inline int aot_blocked(uint32_t phys) {\n"
     "    if (s_aot_block_n == 0) return 0;\n"
     "    if (s_aot_block_n < 0) aot_block_init();\n"))
EXACT.append(("src/fntrace.c",
     "    text_xlate_on_dispatch(cpu, target);\n",
     "#ifndef PSX_WII                         /* no string translation on the Wii */\n"
     "    text_xlate_on_dispatch(cpu, target);\n"
     "#endif\n"))
# Debug rings only the debug server reads: SIO events (the starvation ring
# is compiled out under PSX_NO_DEBUG_TOOLS but SIO still fed it) and BIOS
# vector calls (each also read two guest words for the thread).
EXACT.append(("src/sio.c",
     "static void sr_record(uint8_t kind, uint8_t tx, uint8_t rx) {\n",
     "static void sr_record(uint8_t kind, uint8_t tx, uint8_t rx) {\n"
     "#ifdef PSX_WII\n"
     "    (void)kind; (void)tx; (void)rx;\n"
     "    return;\n"
     "#endif\n"))
EXACT.append(("src/bios_hle.c",
     "static void hle_record(uint32_t vector, uint32_t fn, const CPUState* cpu,\n"
     "                       uint32_t v0, uint8_t route)\n"
     "{\n",
     "static void hle_record(uint32_t vector, uint32_t fn, const CPUState* cpu,\n"
     "                       uint32_t v0, uint8_t route)\n"
     "{\n"
     "#ifdef PSX_WII\n"
     "    (void)vector; (void)fn; (void)cpu; (void)v0; (void)route;\n"
     "    return;\n"
     "#endif\n"))

# Word stores to RAM above the kernel window: none of the BIOS-scratch,
# Tomba card or kernel-bless cases apply there, so skip their tests (and the
# second call and register saves they cost) and keep only the hooks that can
# fire.
EXACT.append(("src/memory.c",
     "static void psx_write_word_raw(uint32_t addr, uint32_t val);\nvoid psx_write_word(uint32_t addr, uint32_t val) {\n"
     "    extern void (*g_overlay_flush_pending_cycles)(void);\n",
     "static void psx_write_word_raw(uint32_t addr, uint32_t val);\n"
     "#ifdef PSX_WII\n"
     "/* The host's poll-loop skip (source/wii_idle.c) can ask whether an\n"
     " * iteration's stores leave memory as it was: while it probes, each store\n"
     " * logs what it overwrites, and psx_poll_probe_settled() checks that every\n"
     " * byte stored to holds its old value again. */\n"
     "extern int g_poll_store_probe, g_poll_store_dirty;\n"
     "extern uint32_t g_poll_dead_lo;\n"
     "#define POLL_LOG_MAX 64\n"
     "static uint32_t s_plog_addr[POLL_LOG_MAX];\n"
     "static uint8_t s_plog_old[POLL_LOG_MAX][4], s_plog_size[POLL_LOG_MAX];\n"
     "static int s_plog_n;\n"
     "void psx_poll_probe_begin(void) { s_plog_n = 0; }\n"
     "int psx_poll_probe_settled(void) {\n"
     "    for (int i = 0; i < s_plog_n; i++)\n"
     "        for (uint32_t b = 0; b < s_plog_size[i]; b++) {\n"
     "            uint32_t a = s_plog_addr[i] + b;\n"
     "            int first = 1;\n"
     "            for (int j = 0; j < i && first; j++)\n"
     "                if (a - s_plog_addr[j] < s_plog_size[j]) first = 0;\n"
     "            if (first && ram[a] != s_plog_old[i][b]) return 0;\n"
     "        }\n"
     "    return 1;\n"
     "}\n"
     "static void psx_poll_store_note(uint32_t phys, int size) {\n"
     "    uint32_t pg = phys >> DIRTY_RAM_PAGE_SHIFT;\n"
     "    if (phys - g_poll_dead_lo < 0x1000u) return;     /* stack below the loop's own frame */\n"
     "    if ((text_ref_image && phys < text_ref_hi && phys + (uint32_t)size > text_ref_lo) ||\n"
     "        (((g_dirty_ram_exec_page_bitmap[pg >> 5] | overlay_watch_bitmap[pg >> 5]) >> (pg & 31u)) & 1u) ||\n"
     "        s_plog_n == POLL_LOG_MAX) {\n"
     "        g_poll_store_dirty = 1;\n"
     "        return;\n"
     "    }\n"
     "    s_plog_addr[s_plog_n] = phys;\n"
     "    s_plog_size[s_plog_n] = (uint8_t)size;\n"
     "    memcpy(s_plog_old[s_plog_n], &ram[phys], (size_t)size);\n"
     "    s_plog_n++;\n"
     "}\n"
     "static inline __attribute__((always_inline)) int psx_write_ram_fast(uint32_t addr, uint32_t val, int size) {\n"
     "    if (addr >= 0xC0000000u || (sr_ptr && (*sr_ptr & 0x10000u))) return 0;\n"
     "    uint32_t phys = psx_phys_addr(addr);\n"
     "    if (phys - DIRTY_RAM_KERNEL_TRACK_BYTES >= RAM_SIZE - DIRTY_RAM_KERNEL_TRACK_BYTES ||\n"
     "        g_card_data_writes_armed || (size == 4 && phys == D44_PHYS)) return 0;\n"
     "    if (__builtin_expect(g_poll_store_probe, 0)) psx_poll_store_note(phys, size);\n"
     "    g_guest_store_count++;\n"
     "    text_guard_note_write(phys, val, size);\n"
     "    overlay_watch_note_write(phys, (uint32_t)size);\n"
     "    if (size == 4) { uint32_t le = __builtin_bswap32(val); memcpy(&ram[phys], &le, 4); }\n"
     "    else if (size == 2) { ram[phys] = (uint8_t)val; ram[phys + 1] = (uint8_t)(val >> 8); }\n"
     "    else ram[phys] = (uint8_t)val;\n"
     "    return 1;\n"
     "}\n"
     "#endif\n"
     "void psx_write_word(uint32_t addr, uint32_t val) {\n"
     "    extern void (*g_overlay_flush_pending_cycles)(void);\n"
     "#ifdef PSX_WII\n"
     "    if (!g_overlay_flush_pending_cycles && g_ls_mode == 0 && !g_ds_recording &&\n"
     "        psx_write_ram_fast(addr, val, 4)) return;\n"
     "    if (g_poll_store_probe && psx_phys_addr(addr) - 0x1F801000u >= 0x2000u) g_poll_store_dirty = 1;\n"
     "#endif\n"))

# Nothing in the Wii build arms the parity ring or the debug server's write
# trace: drop their calls from every guest store.
EXACT.append(("src/memory.c",
     "extern void parity_trace_note_write(uint32_t addr, uint32_t width, uint32_t writer_pc);\n",
     "extern void parity_trace_note_write(uint32_t addr, uint32_t width, uint32_t writer_pc);\n"
     "#ifdef PSX_WII\n"
     "#define parity_trace_note_write(a, w, pc) ((void)0)\n"
     "#define debug_server_trace_write_check(p, o, n, w) ((void)0)\n"
     "#endif\n"))

# Voice loop: only voices active at the start of a block can produce samples
# in it (key-on is a register write, never mid-render), and only voices with a
# volume in sweep mode step a sweep envelope. Walk just those, in voice order.
EXACT.append(("src/spu.c",
     "    int voice_sum_pos = 0;\n",
     "    int voice_sum_pos = 0;\n"
     "    uint32_t act_mask = 0, sweep_mask = 0;\n"
     "    for (int v = 0; v < SPU_VOICE_COUNT; v++) {\n"
     "        if (voices[v].active) act_mask |= 1u << v;\n"
     "        if ((voice_reg(v, 0) | voice_reg(v, 1)) & 0x8000u) sweep_mask |= 1u << v;\n"
     "    }\n"
     "    if (s_shadow_tap_on) act_mask = (1u << SPU_VOICE_COUNT) - 1u;\n"))
EXACT.append(("src/spu.c",
     "            if (any_voice) {\n"
     "                for (int v = 0; v < SPU_VOICE_COUNT; v++) {\n"
     "                    sweep_env_step(&sweep_voice_env[v][0], voice_reg(v, 0));\n",
     "            if (any_voice) {\n"
     "                for (uint32_t m = sweep_mask; m; m &= m - 1u) {\n"
     "                    int v = __builtin_ctz(m);\n"
     "                    sweep_env_step(&sweep_voice_env[v][0], voice_reg(v, 0));\n"))
EXACT.append(("src/spu.c",
     "            if (any_voice) {\n"
     "                for (int v = 0; v < SPU_VOICE_COUNT; v++) {\n"
     "                    int16_t s = voice_next_sample(v);\n",
     "            if (any_voice) {\n"
     "                for (uint32_t m = act_mask; m; m &= m - 1u) {\n"
     "                    int v = __builtin_ctz(m);\n"
     "                    int16_t s = voice_next_sample(v);\n"))

# GTE commands never write the control registers (matrices, vectors, offsets)
# and those round-trip exactly through export and import: keep the state
# across commands and unpack them again only when the guest changed them.
EXACT.append(("src/gte.cpp",
     "static void gte_import_cpu_state(PSXRecomp::GTE::GTEState* gte,\n"
     "                                 const CPUState* cpu) {\n",
     "static void gte_import_cpu_state(PSXRecomp::GTE::GTEState* gte,\n"
     "                                 const CPUState* cpu, int with_ctrl) {\n"))
EXACT.append(("src/gte.cpp",
     "    gte->RT[0][0] = gte_unpack_s16(c[0]);\n",
     "    if (with_ctrl) {\n    gte->RT[0][0] = gte_unpack_s16(c[0]);\n"))
EXACT.append(("src/gte.cpp",
     "    gte->FLAG = c[31] & 0x7FFFF000u;\n",
     "    }\n    gte->FLAG = c[31] & 0x7FFFF000u;\n"))
EXACT.append(("src/gte.cpp",
     "static void gte_export_cpu_state(CPUState* cpu,\n"
     "                                 const PSXRecomp::GTE::GTEState* gte) {\n",
     "static void gte_export_cpu_state(CPUState* cpu,\n"
     "                                 const PSXRecomp::GTE::GTEState* gte, int with_ctrl) {\n"))
EXACT.append(("src/gte.cpp",
     "    c[0] = gte_pack_s16_pair(gte->RT[0][0], gte->RT[0][1]);\n",
     "    if (with_ctrl) {\n    c[0] = gte_pack_s16_pair(gte->RT[0][0], gte->RT[0][1]);\n"))
EXACT.append(("src/gte.cpp",
     "    c[31] = gte->FLAG;\n",
     "    }\n    c[31] = gte->FLAG;\n"))
EXACT.append(("src/gte.cpp",
     "    GTEState gte;\n    gte_import_cpu_state(&gte, cpu);\n",
     "    static GTEState s_gte;\n"
     "    static uint32_t s_gte_ctrl_seen[31];\n"
     "    static int s_gte_ctrl_valid;\n"
     "    GTEState& gte = s_gte;\n"
     "    const int ctrl_changed = !s_gte_ctrl_valid ||\n"
     "        std::memcmp(cpu->gte_ctrl, s_gte_ctrl_seen, sizeof(s_gte_ctrl_seen)) != 0;\n"
     "    gte_import_cpu_state(&gte, cpu, ctrl_changed);\n"))
EXACT.append(("src/gte.cpp",
     "    gte_export_cpu_state(cpu, &gte);\n",
     "    gte_export_cpu_state(cpu, &gte, ctrl_changed);\n"
     "    if (ctrl_changed) {\n"
     "        std::memcpy(s_gte_ctrl_seen, cpu->gte_ctrl, sizeof(s_gte_ctrl_seen));\n"
     "        s_gte_ctrl_valid = 1;\n"
     "    }\n"))

# The same fast path for halfword and byte stores.
for _fn, _ty, _sz in (("psx_write_half", "uint16_t", 2), ("psx_write_byte", "uint8_t", 1)):
    EXACT.append(("src/memory.c",
         "void %s(uint32_t addr, %s val) {\n"
         "    extern void (*g_overlay_flush_pending_cycles)(void);\n" % (_fn, _ty),
         "void %s(uint32_t addr, %s val) {\n"
         "    extern void (*g_overlay_flush_pending_cycles)(void);\n"
         "#ifdef PSX_WII\n"
         "    if (!g_overlay_flush_pending_cycles && g_ls_mode == 0 && !g_ds_recording &&\n"
         "        psx_write_ram_fast(addr, val, %d)) return;\n"
         "    if (g_poll_store_probe && psx_phys_addr(addr) - 0x1F801000u >= 0x2000u) g_poll_store_dirty = 1;\n"
         "#endif\n" % (_fn, _ty, _sz)))

# The guard's change counter, for the host's dispatch caches. A write into a
# text page that holds no compiled code (the EXE's data) changes no range's
# answer, so once the host has registered every program's code ranges only
# writes into code pages count.
EXACT.append(("src/memory.c",
     "static uint32_t text_seq;               /* bumped with any text_page_gen / text_epoch */\n",
     "static uint32_t text_seq;               /* bumped with any text_page_gen / text_epoch */\n"
     "static uint8_t text_code_page[RAM_SIZE >> 12];\n"
     "static int text_code_pages_known;\n"
     "#define TEXT_SEQ_PAGE(pg) do { if (!text_code_pages_known || text_code_page[(pg)]) text_seq++; } while (0)\n"
     "uint32_t dirty_ram_text_seq(void) { return text_seq; }\n"
     "void dirty_ram_text_note_code_range(uint32_t lo, uint32_t len) {\n"
     "    uint32_t phys = lo & 0x1FFFFFFFu;\n"
     "    if (len == 0 || phys >= RAM_SIZE) return;\n"
     "    uint32_t end = len > RAM_SIZE - phys ? RAM_SIZE - 1u : phys + len - 1u;\n"
     "    for (uint32_t pg = phys >> 12; pg <= end >> 12; pg++) text_code_page[pg] = 1;\n"
     "}\n"
     "void dirty_ram_text_code_ranges_done(void) { text_code_pages_known = 1; text_seq++; }\n"))

# Load timing in its steady state. Between two loads in block-timed game code
# nothing touches the pipeline state, so a load usually finds ReadAbsorbWhich 0,
# ReadAbsorb[0] 0 and the previous load pending. There the full sequence
# (base, deps, LDS, absorb reset, fudge) comes down to clearing the slots it
# would clear, setting four fields and charging 1 + 5 cycles, and while the
# block defers its charges the two charges are one addition to the batch.
# Every other state takes the full sequence.
LOAD_SEQ = """        if (g_psx_load_delay) {
            psx_cyc_base(cpu);
            psx_cyc_deps(cpu, reg_mask);
            if (cpu->ld_which_t == rt) cpu->ld_which_t = 0u;
            psx_cyc_lds(cpu);
            cpu->read_absorb[cpu->read_absorb_which] = 0u;
            cpu->read_absorb_which = 0u;
            uint32_t fudge = (uint32_t)((cpu->read_fudge >> 4) & 2u);
            cpu->ld_absorb = 5u;%s
            psx_cyc_charge(fudge + 5u);
            cpu->ld_which_t = (uint8_t)rt;
        }
"""
LOAD_FAST = """static inline __attribute__((always_inline)) int psx_cyc_load_delay_steady(CPUState* cpu,
                                                    uint32_t rt, uint32_t reg_mask) {
    uint32_t lw = cpu->ld_which_t;
    if (cpu->read_absorb_which != 0u || cpu->read_absorb[0] != 0u || lw >= 32u) return 0;
    reg_mask &= 0xFFFFFFFEu;
    while (reg_mask) {
        cpu->read_absorb[__builtin_ctz(reg_mask)] = 0u;
        reg_mask &= reg_mask - 1u;
    }
    if (lw == rt) lw = 0u;
    cpu->read_absorb[lw] = 0u;
    cpu->read_fudge = (uint8_t)lw;
    cpu->ld_absorb = 5u;
    cpu->ld_which_t = (uint8_t)rt;
    if (g_psx_cyc_bb_defer > 0 && !(g_ls_replay_active | g_event_step_conservative) &&
        !psx_in_device_service && !g_psx_cyc_local_acc && g_psx_cyc_batch <= UINT32_MAX - 6u) {
        g_psx_cyc_batch += 6u;
    } else {
        psx_cyc_charge(1u);
        psx_cyc_charge(5u);
    }
    return 1;
}

"""
for _c in (" /* main-RAM wait 3 + completion 2 */", ""):
    EXACT.append(("include/psx_cyc.h", LOAD_SEQ % _c,
                  LOAD_SEQ.replace("        if (g_psx_load_delay) {\n",
                                   "        if (g_psx_load_delay && !psx_cyc_load_delay_steady(cpu, rt, reg_mask)) {\n", 1) % _c))
EXACT.append(("include/psx_cyc.h",
     "/* CPU data load value+timing. Full Beetle sequence for main RAM is inlined;\n",
     LOAD_FAST + "/* CPU data load value+timing. Full Beetle sequence for main RAM is inlined;\n"))

# Guest stores to plain RAM, called directly from block-timed game code
# (scripts/inline_stores.py, source/wii_store.c). A store takes the inline path only where
# psx_write_word/half/byte would do nothing but count it and write the bytes:
# no lockstep, recording or overlay hooks, cache not isolated, main RAM above
# the kernel window, not wholly inside the game text the guard tracks, on a
# page with no executed-code or overlay watch, card capture off. Everything
# else goes to the store hook as before.
EXACT.append(("src/memory.c",
     "void memory_set_sr_ptr(const uint32_t *p) { sr_ptr = p; }\n",
     "const uint32_t *g_mem_sr_ptr;\n"
     "void memory_set_sr_ptr(const uint32_t *p) { sr_ptr = p; g_mem_sr_ptr = p; }\n"))
EXACT.append(("src/memory.c",
     "static uint32_t text_ref_lo = 0, text_ref_hi = 0;\n",
     "static uint32_t text_ref_lo = 0, text_ref_hi = 0;\n"
     "uint32_t g_text_ref_lo, g_text_ref_hi;   /* copies for the inline stores */\n"))
PATCHES.append(("src/memory.c", r"(\n(\s*)text_ref_hi = phys_lo \+ len;\n)",
                r"\1\2g_text_ref_lo = text_ref_lo; g_text_ref_hi = text_ref_hi;\n"))
EXACT.append(("src/memory.c",
     "static uint32_t overlay_watch_bitmap[DIRTY_RAM_BITMAP_WORDS];\n",
     "uint32_t overlay_watch_bitmap[DIRTY_RAM_BITMAP_WORDS];   /* read by the inline stores */\n"))
INLINE_STORES = """#if defined(PSX_WII) && !defined(PSX_OVERLAY_DLL_BUILD)
extern void (*g_overlay_flush_pending_cycles)(void);
extern volatile int g_card_data_writes_armed;
extern const uint32_t *g_mem_sr_ptr;
extern uint32_t g_text_ref_lo, g_text_ref_hi;
extern uint32_t g_dirty_ram_exec_page_bitmap[];
extern uint32_t overlay_watch_bitmap[];
extern uint64_t g_guest_store_count;
extern int g_poll_store_probe;
static inline __attribute__((always_inline)) int psx_store_inline_ok(uint32_t addr, uint32_t size,
                                                                     uint32_t *phys_out) {
    if (g_overlay_flush_pending_cycles || g_ls_mode != 0 || g_ds_recording || g_poll_store_probe ||
        g_card_data_writes_armed || addr >= 0xC0000000u ||
        (g_mem_sr_ptr && (*g_mem_sr_ptr & 0x10000u)))
        return 0;
    uint32_t phys = addr & 0x1FFFFFFFu;
    if (phys >= 0x00800000u) return 0;
    phys &= 0x001FFFFFu;
    if (phys < 0x00010000u) return 0;
    if (phys >= g_text_ref_lo && phys + size <= g_text_ref_hi) return 0;
    uint32_t pg = phys >> 12;
    if (((g_dirty_ram_exec_page_bitmap[pg >> 5] | overlay_watch_bitmap[pg >> 5]) >> (pg & 31u)) & 1u)
        return 0;
    *phys_out = phys;
    return 1;
}
/* One shared function per width (source/wii_store.c), called directly from
 * the generated code: the checks above, the write, or the store hook. */
void psx_store_word_direct(CPUState* cpu, uint32_t addr, uint32_t val);
void psx_store_half_direct(CPUState* cpu, uint32_t addr, uint16_t val);
void psx_store_byte_direct(CPUState* cpu, uint32_t addr, uint8_t val);
#define psx_store_word_inline psx_store_word_direct
#define psx_store_half_inline psx_store_half_direct
#define psx_store_byte_inline psx_store_byte_direct
#endif

"""
EXACT.append(("include/psx_cyc.h",
     "extern uint8_t psx_cyc_load_byte(CPUState* cpu, uint32_t addr, uint32_t rt, uint32_t reg_mask);\n",
     "extern uint8_t psx_cyc_load_byte(CPUState* cpu, uint32_t addr, uint32_t rt, uint32_t reg_mask);\n\n"
     + INLINE_STORES))

# Rectangles in the software GPU: the per-pixel helpers check the buffer
# bounds and the clip rect and reload every draw state global for each pixel.
# The fast paths clip once per rect, hoist the state and the texture page
# lookups, and write the same pixels in the same order.
GPU_RECT_FAST = """#ifdef PSX_WII
static inline void rect_clip(const RTarget *t, int x, int y, int w, int h,
                             int *sx, int *sy, int *ex, int *ey) {
    *sx = max_i(max_i(x, t->cx1), 0);
    *ex = min_i(min_i(x + w - 1, t->cx2), t->w - 1);
    *sy = max_i(max_i(y, t->cy1), 0);
    *ey = min_i(min_i(y + h - 1, t->cy2), t->h - 1);
}

static void raster_flat_rect_fast(const RTarget *t, int x, int y, int w, int h,
                                  uint16_t color) {
    int sx, sy, ex, ey;
    rect_clip(t, x, y, w, h, &sx, &sy, &ex, &ey);
    if (sx > ex || sy > ey) return;
    const int mc = g_mask_check_bit, st = g_semi_trans_enabled, mode = g_semi_trans_mode;
    const uint16_t setb = g_mask_set_bit ? 0x8000u : 0u;
    const int mark = t->buf == g_vram;
    for (int py = sy; py <= ey; py++) {
        uint16_t *row = t->buf + py * t->w;
        int wrote = 0;
        if (!mc && !st) {
            const uint16_t c = (uint16_t)(color | setb);
            for (int px = sx; px <= ex; px++) row[px] = c;
            wrote = 1;
        } else {
            for (int px = sx; px <= ex; px++) {
                uint16_t d = row[px];
                if (mc && (d & 0x8000u)) continue;
                uint16_t c = st ? blend_pixels(d, color, mode) : color;
                row[px] = (uint16_t)(c | setb);
                wrote = 1;
            }
        }
        if (wrote && mark) gpu_vram_dirty_mark_row((uint32_t)py);
    }
}

static void raster_textured_rect_fast(const RTarget *t, int x, int y, int w, int h,
                                      int u, int v, uint16_t clut_x, uint16_t clut_y,
                                      uint16_t texpage) {
    int sx, sy, ex, ey;
    rect_clip(t, x, y, w, h, &sx, &sy, &ex, &ey);
    if (sx > ex || sy > ey) return;
    const int tpx = (texpage & 0xF) * 64, tpy = ((texpage >> 4) & 1) * 256;
    const int depth = (texpage >> 7) & 3;
    const int tw = g_tw_mask_x | g_tw_mask_y;
    const int mux = ~(g_tw_mask_x * 8), oux = (g_tw_off_x & g_tw_mask_x) * 8;
    const int muy = ~(g_tw_mask_y * 8), ouy = (g_tw_off_y & g_tw_mask_y) * 8;
    const int mc = g_mask_check_bit, st = g_semi_trans_enabled, mode = g_semi_trans_mode;
    const int raw = g_raw_texture;
    const uint16_t setb = g_mask_set_bit ? 0x8000u : 0u;
    const int mark = t->buf == g_vram;
    const uint16_t *clut = g_vram + (clut_y & (VRAM_HEIGHT - 1)) * VRAM_WIDTH;
    uint16_t mr[32], mg[32], mb[32];
    if (!raw) {
        for (int i = 0; i < 32; i++) {
            int r = (i * g_mod_r) >> 4, g = (i * g_mod_g) >> 4, b = (i * g_mod_b) >> 4;
            mr[i] = (uint16_t)(r > 31 ? 31 : r);
            mg[i] = (uint16_t)((g > 31 ? 31 : g) << 5);
            mb[i] = (uint16_t)((b > 31 ? 31 : b) << 10);
        }
    }
    for (int py = sy; py <= ey; py++) {
        int tv = (v + (py - y)) & 0xFF;
        if (tw) tv = (tv & muy) | ouy;
        const uint16_t *trow = g_vram + ((tpy + tv) & (VRAM_HEIGHT - 1)) * VRAM_WIDTH;
        uint16_t *row = t->buf + py * t->w;
        int wrote = 0;
        for (int px = sx; px <= ex; px++) {
            int tu = (u + (px - x)) & 0xFF;
            if (tw) tu = (tu & mux) | oux;
            uint16_t texel;
            if (depth == 0) {
                uint16_t wd = trow[(tpx + (tu >> 2)) & (VRAM_WIDTH - 1)];
                texel = clut[(clut_x + ((wd >> ((tu & 3) * 4)) & 0xF)) & (VRAM_WIDTH - 1)];
            } else if (depth == 1) {
                uint16_t wd = trow[(tpx + (tu >> 1)) & (VRAM_WIDTH - 1)];
                texel = clut[(clut_x + ((wd >> ((tu & 1) * 8)) & 0xFF)) & (VRAM_WIDTH - 1)];
            } else {
                texel = trow[(tpx + tu) & (VRAM_WIDTH - 1)];
            }
            if (texel == 0) continue;
            uint16_t d = row[px];
            if (mc && (d & 0x8000u)) continue;
            uint16_t c = raw ? (uint16_t)(texel & 0x7FFFu)
                             : (uint16_t)(mr[texel & 31] | mg[(texel >> 5) & 31] | mb[(texel >> 10) & 31]);
            if (st && (texel & 0x8000u)) c = blend_pixels(d, c, mode);
            row[px] = (uint16_t)(c | setb);
            wrote = 1;
        }
        if (wrote && mark) gpu_vram_dirty_mark_row((uint32_t)py);
    }
}
#endif

static void raster_flat_rect(const RTarget *t, int x, int y, int w, int h,
                             uint16_t color) {
#ifdef PSX_WII
    raster_flat_rect_fast(t, x, y, w, h, color);
    return;
#endif
"""
EXACT.append(("src/gpu_sw_renderer.c",
     "static void raster_flat_rect(const RTarget *t, int x, int y, int w, int h,\n"
     "                             uint16_t color) {\n",
     GPU_RECT_FAST))
EXACT.append(("src/gpu_sw_renderer.c",
     "                                 uint16_t texpage) {\n"
     "    int s = t->s;\n"
     "    for (int row = 0; row < h; row++) {\n",
     "                                 uint16_t texpage) {\n"
     "    int s = t->s;\n"
     "#ifdef PSX_WII\n"
     "    if (s == 1 && !g_texture_filter) {\n"
     "        raster_textured_rect_fast(t, x, y, w, h, u, v, clut_x, clut_y, texpage);\n"
     "        return;\n"
     "    }\n"
     "#endif\n"
     "    for (int row = 0; row < h; row++) {\n"))

# DMA bursts to RAM (the ordering-table clear every frame) wrote each word
# through psx_write_word. Where every word of the range would take its RAM
# fast path and the text and overlay notes have nothing to do there, write
# the words directly.
EXACT.append(("src/memory.c",
     "void psx_write_word(uint32_t addr, uint32_t val) {\n"
     "    extern void (*g_overlay_flush_pending_cycles)(void);\n",
     "#ifdef PSX_WII\n"
     "int psx_ram_bulk_write_ok(uint32_t phys, uint32_t len) {\n"
     "    extern void (*g_overlay_flush_pending_cycles)(void);\n"
     "    if (g_overlay_flush_pending_cycles || g_ls_mode != 0 || g_ds_recording ||\n"
     "        g_card_data_writes_armed || (sr_ptr && (*sr_ptr & 0x10000u))) return 0;\n"
     "    if (len == 0 || phys < DIRTY_RAM_KERNEL_TRACK_BYTES || phys >= RAM_SIZE ||\n"
     "        len > RAM_SIZE - phys) return 0;\n"
     "    if (D44_PHYS >= phys && D44_PHYS - phys < len) return 0;\n"
     "    if (text_ref_image && phys < text_ref_hi && phys + len > text_ref_lo) return 0;\n"
     "    for (uint32_t pg = phys >> DIRTY_RAM_PAGE_SHIFT;\n"
     "         pg <= (phys + len - 1u) >> DIRTY_RAM_PAGE_SHIFT; pg++)\n"
     "        if (((g_dirty_ram_exec_page_bitmap[pg >> 5] | overlay_watch_bitmap[pg >> 5])\n"
     "             >> (pg & 31u)) & 1u) return 0;\n"
     "    return 1;\n"
     "}\n"
     "#endif\n"
     "void psx_write_word(uint32_t addr, uint32_t val) {\n"
     "    extern void (*g_overlay_flush_pending_cycles)(void);\n"))
EXACT.append(("src/dma.c",
     "    uint32_t addr = channels[6].madr & 0x1FFFFCu;\n\n"
     "    for (uint32_t i = 0; i < num_entries; i++) {\n",
     "    uint32_t addr = channels[6].madr & 0x1FFFFCu;\n\n"
     "#ifdef PSX_WII\n"
     "    {\n"
     "        extern int psx_ram_bulk_write_ok(uint32_t phys, uint32_t len);\n"
     "        extern uint8_t *memory_get_ram_ptr(void);\n"
     "        extern uint64_t g_guest_store_count;\n"
     "        uint32_t span = 4u * (num_entries - 1u);\n"
     "        if (addr >= span && psx_ram_bulk_write_ok(addr - span, span + 4u)) {\n"
     "            uint8_t *ram = memory_get_ram_ptr();\n"
     "            for (uint32_t i = 0; i < num_entries; i++, addr -= 4u) {\n"
     "                uint32_t le = __builtin_bswap32(i == num_entries - 1u ? 0x00FFFFFFu\n"
     "                                                : ((addr - 4u) & 0x00FFFFFFu));\n"
     "                memcpy(ram + addr, &le, 4);\n"
     "            }\n"
     "            g_guest_store_count += num_entries;\n"
     "            complete_transfer(6);\n"
     "            return;\n"
     "        }\n"
     "    }\n"
     "#endif\n"
     "    for (uint32_t i = 0; i < num_entries; i++) {\n"))

# Reverb taps: a tap past the end of the work area wrapped with a division
# (the area is rarely a power of two). It is never more than one area out, so
# one add or subtract gives the same offset; the tap helpers are inlined.
EXACT.append(("src/spu.c",
     "        } else {\n"
     "            rel %= (int32_t)size;\n"
     "            if (rel < 0) rel += (int32_t)size;\n"
     "        }\n",
     "        } else if (rel >= (int32_t)size && rel - (int32_t)size < (int32_t)size) {\n"
     "            rel -= (int32_t)size;\n"
     "        } else if (rel < 0 && rel >= -(int32_t)size) {\n"
     "            rel += (int32_t)size;\n"
     "        } else {\n"
     "            rel %= (int32_t)size;\n"
     "            if (rel < 0) rel += (int32_t)size;\n"
     "        }\n"))
EXACT.append(("src/spu.c", "static uint32_t rev_ea(uint32_t reg_addr, int32_t bias) {",
     "static inline __attribute__((always_inline)) uint32_t rev_ea(uint32_t reg_addr, int32_t bias) {"))
EXACT.append(("src/spu.c", "static int16_t rev_read(uint32_t reg_addr, int32_t bias) {",
     "static inline __attribute__((always_inline)) int16_t rev_read(uint32_t reg_addr, int32_t bias) {"))
EXACT.append(("src/spu.c", "static void rev_write(uint32_t reg_addr, int32_t v, int wren) {",
     "static inline __attribute__((always_inline)) void rev_write(uint32_t reg_addr, int32_t v, int wren) {"))

# Lines (Vibri is drawn with them): without the hi-res and wide mirrors, the
# Bresenham walk writes native pixels with the clip box and draw state hoisted.
EXACT.append(("src/gpu_sw_renderer.c",
     "void sw_draw_line(int x0, int y0, int x1, int y1, uint16_t color) {\n"
     "    RTarget n = rt_native();\n",
     "void sw_draw_line(int x0, int y0, int x1, int y1, uint16_t color) {\n"
     "    RTarget n = rt_native();\n"
     "#ifdef PSX_WII\n"
     "    if (!g_hr && !g_wide_cur) {\n"
     "        const int bx1 = max_i(n.cx1, 0), bx2 = min_i(n.cx2, n.w - 1);\n"
     "        const int by1 = max_i(n.cy1, 0), by2 = min_i(n.cy2, n.h - 1);\n"
     "        const int mc = g_mask_check_bit, st = g_semi_trans_enabled, mode = g_semi_trans_mode;\n"
     "        const uint16_t setb = g_mask_set_bit ? 0x8000u : 0u;\n"
     "        const int mark = n.buf == g_vram;\n"
     "        int dx = abs(x1 - x0), dy = -abs(y1 - y0);\n"
     "        int sx = (x0 < x1) ? 1 : -1, sy = (y0 < y1) ? 1 : -1;\n"
     "        int err = dx + dy;\n"
     "        for (;;) {\n"
     "            if (x0 >= bx1 && x0 <= bx2 && y0 >= by1 && y0 <= by2) {\n"
     "                uint16_t *px = n.buf + y0 * n.w + x0;\n"
     "                if (!(mc && (*px & 0x8000u))) {\n"
     "                    uint16_t c = st ? blend_pixels(*px, color, mode) : color;\n"
     "                    *px = (uint16_t)(c | setb);\n"
     "                    if (mark) gpu_vram_dirty_mark_row((uint32_t)y0);\n"
     "                }\n"
     "            }\n"
     "            if (x0 == x1 && y0 == y1) break;\n"
     "            int e2 = 2 * err;\n"
     "            if (e2 >= dy) { err += dy; x0 += sx; }\n"
     "            if (e2 <= dx) { err += dx; y0 += sy; }\n"
     "        }\n"
     "        return;\n"
     "    }\n"
     "#endif\n"))

# The poll-loop skip's probe (source/wii_idle.c): device registers an
# iteration may touch without changing anything a later iteration could see.
EXACT.append(("src/memory.c",
    "static uint32_t mmio_read32(uint32_t addr) {\n"
    "    extern void psx_devices_mmio_sync_read(void);\n",
    "#ifdef PSX_WII\n"
    "extern int g_poll_store_probe, g_poll_store_dirty;\n"
    "extern int cdrom_write_silent(uint32_t addr, uint32_t value);\n"
    "extern int sio_tick0_idle(void);\n"
    "/* Reads: registers whose read changes nothing. */\n"
    "static inline int psx_mmio_pure(uint32_t a) {\n"
    "    if ((a & ~3u) == 0x1F801070u) return sio_tick0_idle();  /* the read ticks the SIO */\n"
    "    if (a >= 0x1F801070u && a < 0x1F801100u) return 1;      /* IRQ and DMA registers */\n"
    "    if (a >= 0x1F801100u && a < 0x1F801130u)                /* timer counters and targets */\n"
    "        return (a & 0xFu) < 4u || ((a & 0xFu) >= 8u && (a & 0xFu) < 12u);\n"
    "    return a == 0x1F801814u || a == 0x1F801800u || a == 0x1F801803u;\n"
    "}\n"
    "/* Writes (after the devices are synced): ones that leave every register as\n"
    " * it was, such as acknowledging interrupts that are not pending. */\n"
    "static inline int psx_mmio_write_silent(uint32_t a, uint32_t v, uint32_t width_mask) {\n"
    "    if (a == 0x1F801070u) return (i_stat & ~v & 0x7FFu & width_mask) == 0;\n"
    "    if (a >= 0x1F801800u && a <= 0x1F801803u) return cdrom_write_silent(a, v);\n"
    "    return 0;\n"
    "}\n"
    "#define POLL_MMIO_READ(a) \\\n"
    "    do { if (__builtin_expect(g_poll_store_probe, 0) && !psx_mmio_pure(a)) g_poll_store_dirty = 1; } while (0)\n"
    "#define POLL_MMIO_WRITE(a, v, m) \\\n"
    "    do { if (__builtin_expect(g_poll_store_probe, 0) && !psx_mmio_write_silent((a), (v), (m))) \\\n"
    "             g_poll_store_dirty = 1; } while (0)\n"
    "#else\n"
    "#define POLL_MMIO_READ(a) do { } while (0)\n"
    "#define POLL_MMIO_WRITE(a, v, m) do { } while (0)\n"
    "#endif\n"
    "static uint32_t mmio_read32(uint32_t addr) {\n"
    "    POLL_MMIO_READ(addr);\n"
    "    extern void psx_devices_mmio_sync_read(void);\n"))
for _ty, _n in (("uint16_t", "16"), ("uint8_t", "8")):
    EXACT.append(("src/memory.c",
        "static %s mmio_read%s(uint32_t addr) {\n"
        "    extern void psx_devices_mmio_sync_read(void);\n" % (_ty, _n),
        "static %s mmio_read%s(uint32_t addr) {\n"
        "    POLL_MMIO_READ(addr);\n"
        "    extern void psx_devices_mmio_sync_read(void);\n" % (_ty, _n)))
for _ty, _n, _m in (("uint32_t", "32", "0xFFFFFFFFu"), ("uint16_t", "16", "0xFFFFu"), ("uint8_t", "8", "0xFFu")):
    EXACT.append(("src/memory.c",
        "static void mmio_write%s(uint32_t addr, %s val) {\n"
        "    psx_devices_mmio_sync();\n" % (_n, _ty),
        "static void mmio_write%s(uint32_t addr, %s val) {\n"
        "    psx_devices_mmio_sync();\n"
        "    POLL_MMIO_WRITE(addr, val, %s);\n" % (_n, _ty, _m)))
EXACT.append(("src/cdrom.c",
    "void cdrom_write(uint32_t addr, uint32_t value) {\n",
    "/* Whether a write, with the devices synced, would change nothing: an index\n"
    " * select, the interrupt enable rewritten, or an acknowledge with nothing\n"
    " * to acknowledge and nothing waiting on one. */\n"
    "int cdrom_write_silent(uint32_t addr, uint32_t value) {\n"
    "    uint8_t val = (uint8_t)value;\n"
    "    if (addr == 0x1F801800u) return 1;\n"
    "    if (index_reg != 1) return 0;\n"
    "    if (addr == 0x1F801802u) return (val & 0x1F) == irq_enable;\n"
    "    if (addr != 0x1F801803u) return 0;\n"
    "    return (irq_flag & val & 0x1F) == 0 && !((val & 0x40) && param_count) &&\n"
    "           !(pending_dataready && pending_present_due == 0) &&\n"
    "           !(queued_cmd.pending && irq_flag == 0) &&\n"
    "           !(pending.pending && psx_cycle_count >= pending.due_cyc && irq_flag == 0);\n"
    "}\n"
    "\n"
    "void cdrom_write(uint32_t addr, uint32_t value) {\n"))

EXACT.append(("src/sio.c",
    "void sio_ape_card_unstick_pump(void) {\n"
    "    if (s_ape_unstick_pending)\n"
    "        ape_card_unstick_maybe(1);\n"
    "}\n",
    "void sio_ape_card_unstick_pump(void) {\n"
    "    if (s_ape_unstick_pending)\n"
    "        ape_card_unstick_maybe(1);\n"
    "}\n"
    "\n"
    "/* Whether sio_tick(0), run by an I_STAT read, would do nothing. */\n"
    "int sio_tick0_idle(void) {\n"
    "    return !(sio_irq_pending && sio_irq_countdown > 0) && !s_ape_unstick_pending;\n"
    "}\n"))

# CD-DA sectors as device events. The next sector (and with it the play
# position GetlocP reports) arrived at whatever device-pass boundary followed
# its due cycle, after any command answered in the same pass. Advertising it
# lands it on its own cycle however passes are cut.
EXACT.append(("src/cdrom.c",
     "    /* Active sector read: next data-ready in read_delay cycles. */\n",
     "    if (cdda_playing && cdda_delay > 0 && (uint32_t)cdda_delay < best)\n"
     "        best = (uint32_t)cdda_delay;\n"
     "    /* Active sector read: next data-ready in read_delay cycles. */\n"))

EXACT.append(("src/interrupts.c",
     "void psx_set_midframe_audio_pump(void (*fn)(void)) { s_midframe_audio_pump = fn; }\n",
     "void psx_set_midframe_audio_pump(void (*fn)(void)) { s_midframe_audio_pump = fn; }\n\n"
     "/* Optional: the cycle at which the host's pumps render the sample `ahead`\n"
     " * samples past its last rendered one, UINT64_MAX if it can't say. */\n"
     "static uint64_t (*s_spu_sample_due)(uint32_t ahead);\n"
     "void psx_set_spu_sample_due(uint64_t (*fn)(uint32_t ahead)) { s_spu_sample_due = fn; }\n"
     "extern uint32_t spu_irq_samples_lower_bound(void);\n"))
EXACT.append(("src/interrupts.c",
     "    g_spu_sample_enabled_queries++;\n",
     "    g_spu_sample_enabled_queries++;\n"
     "    if (s_spu_sample_due) {\n"
     "        uint32_t ahead = spu_irq_samples_lower_bound();\n"
     "        if (ahead == UINT32_MAX) return UINT32_MAX;\n"
     "        uint64_t due = s_spu_sample_due(ahead), now = psx_get_cycle_count();\n"
     "        if (due != UINT64_MAX && due > now)\n"
     "            return due - now > 0xFFFFFFFFull ? UINT32_MAX : (uint32_t)(due - now);\n"
     "    }\n"))

# MEM1 is 24 MB and the generated code is most of it. Arrays the game rarely or
# never touches (debug traces, the BIOS ROM copy, savestate and upload staging
# buffers, memory cards, the CD audio ring) go in a MEM2 section placed by
# mem2.ld; wii_main.c zeroes it before anything runs.
MEM2 = '__attribute__((section(".bss.mem2"))) '
for rel, decl in [
        ("src/spu.c", "static SpuShadowFrameTap s_shadow_tap[SPU_SHADOW_TAP_FRAMES];"),
        ("src/spu.c", "static int16_t  cd_ring[SPU_CD_RING_FRAMES * 2u];"),
        ("src/dirty_ram_interp.c", "static ls_op_t  s_ls_trace[LS_TRACE_CAP];"),
        ("src/mdec.c", "static MDECDebugEvent mdec_trace[MDEC_TRACE_CAP];"),
        ("src/cdrom.c", "static CdTimingRecord s_cd_timing[CD_TIMING_CAP];"),
        ("src/memory.c", "static ImaskTraceEntry imask_trace[IMASK_TRACE_CAP];"),
        ("src/memory.c", "static uint8_t bios_rom[BIOS_ROM_SIZE];"),
        ("src/overlay_loader.c", "static LoadedPair s_loaded_pairs[LOADED_PAIR_CAP];"),
        ("src/overlay_loader.c", "static LazyMan s_lazy_man[LAZY_MAN_CAP];"),
        ("src/overlay_loader.c", "static LazyMissEntry s_lazy_miss_cache[LAZY_MISS_CACHE_CAP];"),
        ("src/memcard.c", "static MemCard cards[MAX_CARDS];"),
        ("src/gpu.c", "static uint16_t vram_write_pixels[1024 * 512];"),
        ("src/boot_state.c", "static uint16_t s_vram_mirror[VRAM_W * VRAM_H];")]:
    EXACT.append((rel, decl + "\n", "static " + MEM2 + decl[len("static "):] + "\n"))

# Later patches edit text that earlier ones inserted, so an already patched
# file can't be told apart reliably: start every patched file from the checkout.
if os.path.isdir(sys.argv[1] + "/.git"):
    files = sorted({"runtime/" + p[0] for p in EXACT + PATCHES})
    subprocess.run(["git", "-C", sys.argv[1], "checkout", "--", *files], check=True)

for rel, old, new in EXACT:
    path = f"{root}/{rel}"
    text = open(path).read()
    if old in text and new not in text:
        open(path, "w").write(text.replace(old, new, 1))
        print(f"patch_runtime: patched {rel}")
    elif new not in text:
        print(f"patch_runtime: WARNING no match in {rel}: {old.splitlines()[0]!r}")

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
