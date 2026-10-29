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
     "        if (!(text_verified_bitmap[c >> 5] & (1u << (c & 31u))))\n"
     "            text_page_gen[phys >> DIRTY_RAM_PAGE_SHIFT]++;\n"
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
     "void (*g_spu_access_hook)(void);\n\n"
     "static void spu_irq_check(uint32_t addr, uint32_t len) {\n"))
for sig in ("uint32_t spu_read(uint32_t addr) {\n",
            "void spu_write(uint32_t addr, uint32_t value) {\n",
            "void spu_dma_write(uint32_t word) {\n",
            "uint32_t spu_dma_read(void) {\n"):
    EXACT.append(("src/spu.c", sig, sig + "    if (g_spu_access_hook) g_spu_access_hook();\n"))
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
     "uint32_t spu_irq_samples_lower_bound(void) {\n"
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
