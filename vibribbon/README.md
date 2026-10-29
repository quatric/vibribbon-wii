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

`gen.sh` reads the boot executable, `MAIN_T.EXE` (title) and `MAIN_G.EXE` (game)
from the disc image, runs the recompiler on each, recompiles the BIOS and builds
libwiiinput. Everything lands in `build/`. `MAIN_K.EXE` (the kiosk demo build) is
not recompiled and would run in the runtime's interpreter.

The game is validated on Sony's SCPH-5552 (Europe) BIOS. Pass your own dump as
the second argument; it must be 524288 bytes with SHA-256
`1faaa18fa820a0225e488d9f086296b8e6c46df739666093987ff7d8fd352c09`. Without it
the build falls back to OpenBIOS (MIT licensed), which does not start this disc.

### Timing mode

By default the recompiled code charges cycles per instruction, the framework's
cycle-exact timing model. `VIB_BLOCK_CYCLES=1 scripts/gen.sh ...` charges each
basic block up front instead: about 25% faster on the Wii and 2 MB smaller, but
instruction-cache and mid-block multiply/divide/GTE stall timing are no longer
exact. The BIOS always stays cycle-exact.

## SD card layout

```
sd:/apps/vibribbon/boot.dol
sd:/apps/vibribbon/bios.bin               (build/bios.bin)
sd:/apps/vibribbon/text.bin               (build/text.bin, MAIN_G reference text)
sd:/apps/vibribbon/text_t.bin             (build/text_t.bin, MAIN_T reference text)
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
which makes headless Dolphin runs inspectable. `make EXIT_SECONDS=N` adds a
watchdog log, and `make PROFILE=N` samples where the CPU is, starting N seconds
in, into `prof.bin` (pc, lr and guest pc per sample, big-endian).

## Status

Boots with the SCPH-5552 BIOS and runs through the NanaOn-Sha logo and the intro
to the title screen in Dolphin, at about 10 frames per second (the PS1 runs at 50)
in the default cycle-exact mode.
Not yet playable.

Done:
- The boot loader, `MAIN_T.EXE` and `MAIN_G.EXE` recompile statically. Each is
  partially linked into its own object and they are combined in
  `source/wii_dispatch.c`; the two main programs share an address range, so each
  has its own reference text for the runtime's native-code guard.
- Byte order: guest RAM stays little-endian and the runtime's native loads and
  CD/MDEC DMA paths byte-swap (`scripts/patch_runtime.py`).
- Wii host layer: video out (VRAM to the TV through a YCbCr lookup table), disc
  reader over SD with a CUE parser, libwiiinput controller mapping, SDL shim over
  libogc, runtime stubs. The guest runs on its own thread with a 1 MB stack.

Not done:
- Speed. Most time goes to the runtime's cycle-exact timing model (the
  per-instruction step, timed loads, interrupt checks and device servicing), not
  to the game code. Inlining the step everywhere would not fit in MEM1.
- Frame pacing for when the emulation runs faster than real time.
- Audio output (SPU to the Wii audio interface).
- Reading CD audio tracks from a USB drive (the original's level generation),
  reusing `source/usbcd.c` from the parent project.
- libwiiupdate self-update hook.
- Testing on real hardware.

MEM1 is tight: the build is about 18.6 MB of the Wii's 24 MB, and it needs
roughly 2.5 MB free for its heap. Generated code is built at `-Os` for that reason.
