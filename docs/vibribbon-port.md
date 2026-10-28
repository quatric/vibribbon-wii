# Vib-Ribbon port notes

Goal: a Wii homebrew build of Vib-Ribbon that reads the game's data from the
user's own disc image on SD/USB and builds levels from audio CDs in the USB
drive, reusing `usbcd.c` and the CD-DA pipeline from this repo. No game data
is ever stored in this repository.

## What the disc contains (PAL, SCES-02873)

| File | Notes |
|---|---|
| `MAIN_G.EXE` | PS-X EXE, entry `0x8005405c`, text at `0x80019000`, 0x5d800 bytes. C++ built with the PsyQ SDK |
| `GAME/*_FILES.PAK` | 242 entries each, one pack per language/region variant |
| `TITLE/FILES.PAK`, `KIOSK/*` | title screen and demo assets |
| `GAME/AUDIO/PSJ_SE.VH/.VB` | SPU sound effects (VAB bank) |
| tracks 2-8 | CD-DA music |

PAK layout: `u32 count`, `u32 offsets[count]`; each entry is a NUL-terminated
name padded to 4 bytes, `u32 size`, then the payload. `tools/vibpak.py`
lists and extracts them straight from the MODE2/2352 data track.

Payload types seen: `.TMD` (PS1 models, line primitives for the vector look),
`.ANM` and `.ANC` (animation and camera tracks), `.FSL` (a Lisp-like script,
`SCRIPT/SYSTEM3.FSL`, parsed by `Forms.cpp`), `PIF`.

## Strategy

Reimplement the engine rather than recompile the MIPS code.

1. Loader: ISO/PAK reader (done in Python, port to C), TMD/ANM/ANC parsers.
2. Renderer: draw TMD line primitives with GX as additive white lines.
3. Level generation: the original analyses the CD audio in `SignalModule.cpp`
   and `AnalysisScript.cpp`. Reverse that (rea on `MAIN_G.EXE`) or write an
   equivalent onset/band analysis on the PCM that `cdplayer.c` already reads.
4. Gameplay: obstacle types (`N01_J` .. `N10_LW`), evolution of the character
   (Frog, Snake, Peeloo, Super), speed/time meters, scoring.
5. Input: libwiiinput for Wii Remote, Classic, GameCube pad, keyboard.
6. Updates: libwiiupdate for self-update from the Homebrew Channel.

## Next steps

- Disassemble `SignalModule` and the road generator to pin down the audio
  to obstacle mapping.
- Write the TMD/ANM decoder and a GX line renderer; check against the PS1
  look with `rea` captures.
