#!/usr/bin/env bash
# Generate the recompiled C for the Wii build from your own Vib-Ribbon disc.
#
#   scripts/gen.sh "/path/to/Vib-Ribbon (Europe) (En,Fr,De,Es,It) (Track 1).bin"
#
# Everything lands in build/ (git-ignored). Nothing from the disc is stored in
# this repository.
set -euo pipefail

BIN="${1:?usage: gen.sh <Track 1.bin>}"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
B="$HERE/build"
FW="$B/psxrecomp"
GAME="$B/game-recomp"
EXTRACT="$B/disc"
GEN="$B/gen"

FW_REPO="https://github.com/Alexbeav/psxrecomp.git"
FW_COMMIT="799e74c6ad347d1b292166de7e0fecc8fa6e17be"
GAME_REPO="https://github.com/alexbeavs-ps1-ports/vib-ribbon-recomp.git"

mkdir -p "$B" "$EXTRACT" "$GEN/boot" "$GEN/main" "$GEN/title"

if [ ! -d "$FW/.git" ]; then
    git clone -q "$FW_REPO" "$FW"
fi
git -C "$FW" fetch -q origin "$FW_COMMIT" 2>/dev/null || true
git -C "$FW" checkout -q "$FW_COMMIT"

# Fit the runtime in the Wii's 24 MB and fix its little-endian host assumptions.
python3 -I "$HERE/scripts/patch_runtime.py" "$FW"

if [ ! -d "$GAME/.git" ]; then
    git clone -q --depth 1 "$GAME_REPO" "$GAME"
fi

# OpenBIOS (MIT) is missing from the fork; take it from upstream.
if [ ! -f "$FW/bios/openbios.bin" ]; then
    gh api repos/mstan/psxrecomp/contents/bios/openbios.bin \
        -H "Accept: application/vnd.github.raw" > "$FW/bios/openbios.bin"
fi

if [ ! -x "$FW/recompiler/build/psxrecomp-game" ]; then
    cmake -S "$FW/recompiler" -B "$FW/recompiler/build" -G Ninja -DCMAKE_BUILD_TYPE=Release >/dev/null
    cmake --build "$FW/recompiler/build"
fi

python3 -I "$ROOT/tools/vibpak.py" "$BIN" extract "$EXTRACT" >/dev/null

R="$FW/recompiler/build/psxrecomp-game"
# Timing: by default every instruction charges its own cycles (the framework's
# cycle-exact model). VIB_BLOCK_CYCLES=1 charges each basic block up front
# instead: much faster on the Wii, but instruction-cache and mid-block
# multiply/divide/GTE stall timing are no longer exact.
if [ "${VIB_BLOCK_CYCLES:-0}" = 1 ]; then
    export PSX_CODEGEN_CYCLE_PER_INSN=0
fi
"$R" "$EXTRACT/SCES_028.73" --project-root "$FW" --seeds "$GAME/seeds/ghidra_funcs.txt" --out-dir "$GEN/boot" --strict >/dev/null
"$R" "$EXTRACT/MAIN_G.EXE" --project-root "$FW" --out-dir "$GEN/main" --strict >/dev/null
"$R" "$EXTRACT/MAIN_T.EXE" --project-root "$FW" --out-dir "$GEN/title" --strict >/dev/null
python3 -I "$HERE/scripts/patch_gen.py" "$GEN"/boot/*_dispatch.c "$GEN"/main/*_dispatch.c "$GEN"/title/*_dispatch.c >/dev/null
if [ "${VIB_BLOCK_CYCLES:-0}" = 1 ]; then
    # and takes interrupts at backward branches, calls and returns only
    python3 -I "$HERE/scripts/irq_backedges.py" "$GEN"/boot/*_full_*.c "$GEN"/main/*_full_*.c "$GEN"/title/*_full_*.c
fi

# BIOS: the game is validated on Sony's SCPH-5552 (PAL). Pass your own dump as
# the second argument to use it; without one the build falls back to OpenBIOS.
SCPH5552_SHA256="1faaa18fa820a0225e488d9f086296b8e6c46df739666093987ff7d8fd352c09"
BIOS_DUMP="${2:-}"
if [ -n "$BIOS_DUMP" ]; then
    got="$(shasum -a 256 "$BIOS_DUMP" | cut -d' ' -f1)"
    if [ "$got" != "$SCPH5552_SHA256" ]; then
        echo "gen.sh: $BIOS_DUMP is not an SCPH-5552 dump (sha256 $got)" >&2
        exit 1
    fi
    cp "$BIOS_DUMP" "$FW/bios/SCPH5552.BIN"
    BIOS_STEM=SCPH5552
    BIOS_ROM="$FW/bios/SCPH5552.BIN"
else
    BIOS_STEM=OpenBIOS
    BIOS_ROM="$FW/bios/openbios.bin"
fi
(cd "$FW" && env -u PSX_CODEGEN_CYCLE_PER_INSN bash tools/regen_bios.sh --config "bios/$BIOS_STEM.toml" >/dev/null)
rm -f "$GEN"/bios/*.c
mkdir -p "$GEN/bios"
cp "$FW"/generated/"$BIOS_STEM"_*.c "$GEN/bios/"
cat > "$GEN/bios/bios_registry.c" <<EOF
#include "psx_bios_backend.h"
extern const PsxBiosBackend ${BIOS_STEM}_psx_bios_backend;
const PsxBiosBackend *const psx_bios_registry[] = { &${BIOS_STEM}_psx_bios_backend };
const uint32_t psx_bios_registry_count = 1u;
EOF
cp "$BIOS_ROM" "$B/bios.bin"

# Reference text images for the runtime's native-code guard, one per main
# program (MAIN_G and MAIN_T load at the same address): the boot loader's and
# that program's text in one range, big-endian header {phys_lo, len}, then bytes.
for pair in "MAIN_G.EXE text.bin" "MAIN_T.EXE text_t.bin"; do
python3 -I - "$EXTRACT/SCES_028.73" "$EXTRACT/${pair% *}" "$B/${pair#* }" <<'PY'
import struct, sys
parts = []
for path in sys.argv[1:3]:
    d = open(path, "rb").read()
    addr, size = struct.unpack_from("<II", d, 0x18)
    parts.append((addr & 0x1FFFFFFF, d[0x800:0x800 + size]))
lo = min(a for a, _ in parts)
hi = max(a + len(b) for a, b in parts)
img = bytearray(hi - lo)
for a, b in parts:
    img[a - lo:a - lo + len(b)] = b
open(sys.argv[3], "wb").write(struct.pack(">II", lo, len(img)) + bytes(img))
PY
done

# USB/Wii/GameCube/Classic input and self-update libraries (devkitPPC).
export DEVKITPRO="${DEVKITPRO:-/opt/devkitpro}" DEVKITPPC="${DEVKITPPC:-/opt/devkitpro/devkitPPC}"
for lib in libwiiinput libwiiupdate; do
    if [ ! -d "$B/$lib/.git" ]; then
        git clone -q "https://github.com/quatric/$lib.git" "$B/$lib"
    fi
done
make -C "$B/libwiiinput" >/dev/null

echo "generated:$(ls "$GEN"/boot/*.c "$GEN"/main/*.c "$GEN"/title/*.c "$GEN"/bios/*.c | wc -l) C files in $GEN"
