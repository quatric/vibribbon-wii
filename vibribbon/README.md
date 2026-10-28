# Vib-Ribbon for Wii

The PAL PlayStation game, statically recompiled to C and run on the Wii through
the [PSXRecomp](https://github.com/Alexbeav/psxrecomp) runtime. Controllers go
through [libwiiinput](https://github.com/quatric/libwiiinput) and the app can
update itself with [libwiiupdate](https://github.com/quatric/libwiiupdate).

**Nothing from the game is in this repository.** You supply your own disc image;
the C is generated from it on your machine.

## Build

Needs devkitPPC, libogc, CMake, Ninja, Python 3 and `gh`.

```sh
scripts/gen.sh "/path/to/Vib-Ribbon (Europe) (En,Fr,De,Es,It) (Track 1).bin" [/path/to/SCPH5552.BIN]
make
```

`gen.sh` reads the boot executable and `MAIN_G.EXE` from the disc image, runs the
recompiler on both, recompiles the BIOS and builds libwiiinput. Everything lands
in `build/`.

The game is validated on Sony's SCPH-5552 (Europe) BIOS. Pass your own dump as
the second argument; it must be 524288 bytes with SHA-256
`1faaa18fa820a0225e488d9f086296b8e6c46df739666093987ff7d8fd352c09`. Without it
the build falls back to OpenBIOS (MIT licensed), which boots but does not yet
start the game.

## SD card layout

```
sd:/apps/vibribbon/boot.dol
sd:/apps/vibribbon/bios.bin               (build/bios.bin)
sd:/apps/vibribbon/text.bin               (build/text.bin)
sd:/apps/vibribbon/disc/Vib-Ribbon (Europe) (En,Fr,De,Es,It).cue
sd:/apps/vibribbon/disc/... (Track N).bin  all eight, as referenced by the cue
```

## Controls

| Wii Remote | Action |
|---|---|
| D-pad | D-pad |
| A / B / 1 / 2 | Cross / Circle / Square / Triangle |
| + / - | Start / Select |

Classic Controller, GameCube pads and USB keyboards map the same way through
libwiiinput.

## Debugging

The host writes `log.txt` and screenshots (`dump_*.ppm`) into the app folder,
which makes headless Dolphin runs inspectable.

## Status

Builds and boots to the point of entering the runtime. Not yet playable.

Done:
- Both game programs (boot loader and `MAIN_G.EXE`) recompile statically and
  compile for PowerPC (about 2.4 MB of code).
- OpenBIOS is recompiled and linked, so no console BIOS dump is needed.
- Wii host layer: video out (VRAM to the TV), disc reader over SD with a CUE
  parser, libwiiinput controller mapping, SDL shim over libogc, runtime stubs.

Not done:
- **Byte order.** The PS1 is little-endian, the Wii is big-endian. The runtime
  reads guest RAM with native loads in places (`memory.c`, the SPU and VRAM
  copy paths). Those must go through byte-swapping accessors before the game
  can run correctly.
- Audio output (SPU to the Wii audio interface).
- Reading CD audio tracks from a USB drive (the original's level generation),
  reusing `source/usbcd.c` from the parent project.
- libwiiupdate self-update hook.
- Verification on hardware or in an emulator; headless emulator runs have not
  yet produced a game log.
