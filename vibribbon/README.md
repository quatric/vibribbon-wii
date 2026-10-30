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
not recompiled and would run in the runtime's interpreter. `scripts/patch_runtime.py`
adapts the runtime to the Wii and `scripts/patch_gen.py` the generated dispatch
tables; both are applied by `gen.sh`.

The game is validated on Sony's SCPH-5552 (Europe) BIOS. Pass your own dump as
the second argument; it must be 524288 bytes with SHA-256
`1faaa18fa820a0225e488d9f086296b8e6c46df739666093987ff7d8fd352c09`. Without it
the build falls back to OpenBIOS (MIT licensed), which does not start this disc.

### Timing mode

By default the recompiled code charges cycles per instruction, the framework's
cycle-exact timing model, and every speed-up in the default build leaves frames
and cycle counts unchanged. Two opt-ins trade exactness for speed:

- `VIB_BLOCK_CYCLES=1 scripts/gen.sh ...` charges each basic block up front:
  instruction-cache and mid-block multiply/divide/GTE stall timing are no longer
  exact. It also takes interrupts at backward branches, calls and returns only
  (`scripts/irq_backedges.py`), so one can wait out a run of straight-line
  code. The BIOS always stays cycle-exact.
- `make FAST=1` walks GPU linked-list DMA lazily against a completion predicted
  at its start, instead of one device event per packet. A packet the CPU
  rewrites after the DMA started is read late. It also lets a device-register
  read go without catching the devices up while no device event is due and they
  are under 512 cycles behind, so a polled counter or status bit can read that
  much late (the pad routine polls the SIO, timer 1 and I_STAT for every byte).

In a course the two together run about 2.1x the default speed, which is full
speed in Dolphin.

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

The host writes `log.txt` into the app folder, which makes headless Dolphin
runs inspectable. `make EXIT_SECONDS=N` adds a watchdog log and a screenshot
(`dump_*.ppm`) every 300 frames (`SCREENSHOTS=0` leaves them out, for timing), and `make PROFILE=N` samples where the CPU is, starting N seconds
in, into `prof.bin` (pc, lr and guest pc per sample, big-endian).
`make AUTOPLAY=900` presses Start and Cross on a schedule from frame 900, which
takes a run through the menus into the first Bronze course.
`make WAVDUMP=N` records 6 s of the audio output from frame N into `audio.wav`.

## Status

Boots with the SCPH-5552 BIOS and runs the NanaOn-Sha logo, intro, title, menus
and the first Bronze course with its music, in Dolphin. Not yet tested on a Wii.

Speed in Dolphin, whose timing approximates the Wii's CPU (the PS1 runs at 50
frames per second):

| | default (cycle-exact) | `VIB_BLOCK_CYCLES=1` + `FAST=1` |
|---|---|---|
| boot, intro, title | 50 fps | 50 fps, ~40 fps for the first second |
| menus | ~19 fps | 49-50 fps |
| loading | | 50 fps, ~40 fps for one transition |
| a course | ~23 fps | 50 fps |

The game waits for vblank in PsyQ's `v_wait` loop for most of an idle frame,
and for the CD in libcd's `CdSync()`/`CdReady()` and the BIOS's event loops;
`source/wii_idle.c` skips those waits without changing any guest-visible
timing. A loop that stores or reads device registers is run once with the
runtime watching it, and skipped only when its stores leave memory as it was and
its device accesses change nothing. What remains in a course is the runtime's faithful model of the PS1:
per-instruction timing, a device pass per DMA packet, SIO and timer polling,
and the SPU rendered per sample while its IRQ is armed. The game's own code is
under 10% of the time.

Done:
- The boot loader, `MAIN_T.EXE` and `MAIN_G.EXE` recompile statically. Each is
  partially linked into its own object and they are combined in
  `source/wii_dispatch.c`; the two main programs share an address range, so each
  has its own reference text for the runtime's native-code guard.
- Byte order: guest RAM stays little-endian and the runtime's native loads and
  CD/MDEC DMA paths byte-swap (`scripts/patch_runtime.py`).
- Wii host layer: video out (VRAM to the TV through a YCbCr lookup table, every
  2nd frame when running at full speed, every 4th otherwise), SPU audio through
  the audio interface (44.1 to 48 kHz), frame pacing on the audio queue, disc
  reader over SD with a CUE parser, libwiiinput controller mapping, SDL shim over
  libogc, runtime stubs. The guest runs on its own thread with a 1 MB stack.

Not done:
- Full speed in the cycle-exact default build, and in the two slow seconds above.
- Reading CD audio tracks from a USB drive (the original's level generation),
  reusing `source/usbcd.c` from the parent project.
- libwiiupdate self-update hook.
- Testing on real hardware.

MEM1 is tight. The generated code is built at `-O2` and takes most of it, so
the runtime's cold arrays (debug traces, the BIOS ROM copy, savestate and
upload staging, memory cards, the CD audio ring, about 6 MB) are placed at the
bottom of MEM2 by `ld/rvl.ld`. MEM1 holds about 18.4 MB (16.1 MB with
`VIB_BLOCK_CYCLES=1`), leaving the rest for the heap.
