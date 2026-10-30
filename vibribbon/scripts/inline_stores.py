#!/usr/bin/env python3
"""Post-process block-timed game code (build/gen/<program>/*_full_*.c) so
guest stores call a direct store function.

Each store calls the runtime through cpu->write_word/half/byte, whose hooks
check lockstep, recording, overlay, kernel, text-guard and watch state before
writing two to four bytes. psx_store_*_inline (source/wii_store.c, checks in
include/psx_cyc.h from scripts/patch_runtime.py) makes the same checks without
the hook's frame and calls the hook only where one of them applies. Used with
VIB_BLOCK_CYCLES=1.
"""
import re
import sys

STORE = re.compile(r'\bcpu->write_(word|half|byte)\(')

count = 0
for path in sys.argv[1:]:
    text = open(path).read()
    text, n = STORE.subn(lambda m: f'psx_store_{m.group(1)}_inline(cpu, ', text)
    count += n
    open(path, 'w').write(text)
print(f"inline_stores: {count} stores")
