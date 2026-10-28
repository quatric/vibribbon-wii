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

mkdir -p "$B" "$EXTRACT" "$GEN/boot" "$GEN/main"

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
"$R" "$EXTRACT/SCES_028.73" --project-root "$FW" --seeds "$GAME/seeds/ghidra_funcs.txt" --out-dir "$GEN/boot" --strict >/dev/null
"$R" "$EXTRACT/MAIN_G.EXE" --project-root "$FW" --out-dir "$GEN/main" --strict >/dev/null

(cd "$FW" && bash tools/regen_bios.sh --config bios/OpenBIOS.toml >/dev/null)
mkdir -p "$GEN/bios"
cp "$FW"/generated/OpenBIOS_*.c "$GEN/bios/"

# Reference text image for the runtime's native-code guard: the text of both
# programs in one range, big-endian header {phys_lo, len} then the bytes.
python3 -I - "$EXTRACT/SCES_028.73" "$EXTRACT/MAIN_G.EXE" "$B/text.bin" <<'PY'
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

# USB/Wii/GameCube/Classic input and self-update libraries (devkitPPC).
export DEVKITPRO="${DEVKITPRO:-/opt/devkitpro}" DEVKITPPC="${DEVKITPPC:-/opt/devkitpro/devkitPPC}"
for lib in libwiiinput libwiiupdate; do
    if [ ! -d "$B/$lib/.git" ]; then
        git clone -q "https://github.com/quatric/$lib.git" "$B/$lib"
    fi
done
make -C "$B/libwiiinput" >/dev/null

echo "generated:$(ls "$GEN"/boot/*.c "$GEN"/main/*.c "$GEN"/bios/*.c | wc -l) C files in $GEN"
