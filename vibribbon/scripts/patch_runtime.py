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
for rel, old, new in EXACT:
    path = f"{root}/{rel}"
    text = open(path).read()
    if old in text:
        open(path, "w").write(text.replace(old, new, 1))
        print(f"patch_runtime: byte-order fix in {rel}")

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
