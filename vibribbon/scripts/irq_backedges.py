#!/usr/bin/env python3
"""Post-process recompiled game code (build/gen/<program>/*_full_*.c) so it
takes interrupts at backward branches, calls and returns only.

The recompiler checks for interrupts on both edges of every branch. A forward
branch inside a function can't loop, so dropping its check delays a pending
interrupt by at most the straight-line code up to the next backward branch,
call or return, where the check still runs. Used with VIB_BLOCK_CYCLES=1,
whose timing is already block-granular; the default build keeps every check.
"""
import re
import sys

LABEL = re.compile(r'^(?:block_([0-9A-F]{8}):|void (?:\w+_)?func_([0-9A-F]{8})\(CPUState)')
CHECK = re.compile(r'^\s*psx_check_interrupts_at\(cpu, 0x([0-9A-F]{8})u\);\s*$')
GOTO = re.compile(r'^\s*goto block_([0-9A-F]{8});')

dropped = 0
for path in sys.argv[1:]:
    lines = open(path).read().split('\n')
    out, cur = [], None
    for i, line in enumerate(lines):
        m = LABEL.match(line)
        if m:
            cur = int(m.group(1) or m.group(2), 16)
        m = CHECK.match(line)
        if m and cur is not None and i + 1 < len(lines):
            g = GOTO.match(lines[i + 1])
            if g and g.group(1) == m.group(1) and int(m.group(1), 16) > cur:
                dropped += 1
                continue
        out.append(line)
    open(path, 'w').write('\n'.join(out))
print(f"irq_backedges: dropped {dropped} forward-branch checks")
