# Wii CD Player

Homebrew app that plays audio CDs from a USB CD/DVD drive plugged into the Wii
(developed against an LG GP60NS50 slim drive).

## How it works

```
USB drive ──BOT/SCSI──> usbcd.c ──READ CD 0xBE──> cdplayer.c ring (32 × 8 sectors)
                                                      │
                                      audio thread ──> 44.1 → 48 kHz sinc resampler
                                                      ──> AI DMA, 16-bit PCM
```

* **`source/usbcd.c`** is a standalone USB Mass Storage Bulk-Only Transport
  driver built on libogc's `USB_*` IOS calls. libogc's own `usbstorage` can't be
  used here: it only does READ(10)/WRITE(10) and caps the CBW CDB length at 10
  bytes, while CD audio needs 12-byte MMC commands (`READ CD`, `SET CD SPEED`).
  It finds the first mass-storage device whose INQUIRY says *peripheral type 5*
  (CD/DVD), so a USB stick or HDD in the other port is ignored. It reads the TOC
  and CD-TEXT, pulls raw 2352-byte CD-DA frames, and handles eject, speed and
  sense decoding. Every transfer has a timeout and does BOT reset recovery on
  failure.
* **`source/cdplayer.c`** has a drive thread that polls for the drive and disc,
  runs commands and reads ahead about 3.4 s. The audio thread feeds the
  audio interface's DMA directly, without ASND. The AI only plays 32 or
  48 kHz, so the CD's 44.1 kHz is converted with a 64-tap polyphase
  windowed-sinc filter. Resampling images stay about 80 dB down, where ASND's
  cheap resampler let them fold back into the audible band as grit. If a read
  fails, that chunk plays as silence and playback continues.
* **`source/metadata.c`** names the disc. It computes the MusicBrainz disc ID
  from the TOC, checks the SD cache (`sd:/apps/cdplayer/cache/<discid>.txt`),
  and otherwise asks MusicBrainz over HTTPS. If the exact disc ID
  isn't in the database, MusicBrainz's fuzzy TOC search is used instead, and
  the disc whose TOC is closest is picked. That matters for multi-disc sets
  where every disc has the same number of tracks. MusicBrainz names take
  priority over CD-TEXT, and CD-TEXT fills any gaps. Accents and curly quotes
  are converted to ASCII because the console font has no other characters.
* **`source/http.c`** is a small blocking HTTP/1.1 client on libogc's
  `net_*` sockets, with TLS 1.2 from mbedTLS. It verifies the certificate
  chain and host name against the roots in `source/certs.h`. Certificate dates
  aren't checked, because many Wii clocks are wrong.
* **`source/netlog.c`** keeps a debug log, mirrors it to `sd:/cdplayer.log` and
  serves it with a small command shell on TCP port 5555.
* **`source/main.c`** draws the console UI on the TV and handles input from the
  Wii Remote and GameCube controller.

## Build

```bash
make dist
```

`dist/apps/cdplayer/` (`boot.dol`, `meta.xml`, `icon.png`) goes on the SD card
under `/apps/`. You need devkitPPC, libogc and Python 3
with Pillow (for the icon).

The first build runs `tools/build-mbedtls.sh`. It downloads mbedTLS 3.6.7,
checks the tarball against `tools/mbedtls.sha256`, and cross-compiles a static
library configured by `config/mbedtls_wii.h` into `third_party/wii/`. Run it
with `--force` to rebuild.

## Fast iteration over the network

Open the Homebrew Channel on the Wii, then from the Mac:

```bash
WIILOAD=tcp:<wii-ip> make run
```

## Controls

| Action | Wii Remote | GameCube |
|---|---|---|
| Play / pause (plays the highlighted track if different) | A | A |
| Stop · when stopped: eject | B | B (Z = eject) |
| Prev / next track · hold to scan ±5 s | ◀ / ▶ | D-pad ◀ / ▶ |
| Move highlight | ▲ / ▼ | D-pad ▲ / ▼ |
| Volume | + / − | R / L |
| Repeat (off / all / one) · shuffle | 1 · 2 | X · Y |
| Exit | HOME | START |

## Debugging with the GP60NS50

Once the network comes up, the bottom line on the TV shows the Wii's IP.

```bash
nc <wii-ip> 5555
```

You see the whole log so far, then new lines as they happen. Try these
commands, in this order:

1. `status`: should say `drive: ... [HL-DT-ST DVD... GP60NS50 ...] high-speed`.
   If it says `none`, the Wii never enumerated the drive (see below).
2. `inquiry` and `tur`: `tur` decodes the sense (no disc / tray open /
   becoming ready).
3. `toc`: the track list the drive reports.
4. `read 0 4`: reads 4 raw sectors from LBA 0 and prints the timing, peak
   level and first bytes. On silence at the start of a track, try a few
   seconds in, e.g. `read 2000 4`.
5. `play 1`, `status`: check the buffer fill, underruns and average read time.
6. `meta`: shows the disc ID, the lookup state and the names it found.
   `meta refresh` skips the cache and asks MusicBrainz again. `lookup FIRST
   LAST LEADOUT OFFSETS...` looks up any TOC, even with no drive attached.
7. `cdb ...`: sends any raw SCSI command, e.g.
   `cdb 43 00 00 00 00 00 00 03 24 00 len=804` (READ TOC) or
   `cdb 4a 01 00 00 10 00 00 00 08 00 len=8` (GET EVENT STATUS, media).

If you have no network, the same log is written to `sd:/cdplayer.log`.

### Things likely to need tuning on real hardware

* **Power.** The GP60NS50 is bus-powered, and the Wii's ports are only
  specified for 500 mA. If the drive clicks, spins down or drops off the bus
  during spin-up, use a Y-cable into both Wii ports or a powered hub.
* **Left and right swapped, or no sound.** Type `tone` in the shell. It plays
  440 Hz on the left and 1 kHz on the right, through the same resampler and
  DMA path as the CD. Type `tone` again to stop it.
* **Noise / static instead of music.** The drive is returning big-endian
  samples. Type `swap` in the shell. If that fixes it, the default should be
  flipped for this drive.
* **Stutter / underruns.** Run `speed 0` (drive max) or `speed 1411` (8×). The
  default is 4× (706 kB/s) because it's quieter.
* **`drive: none`.** Check the log for IOS58. The Homebrew Channel normally
  starts apps under IOS58, which is needed for USB 2.0 (EHCI). Also try the
  other USB port.
* **Tray won't close from the app.** Slim drives have spring-loaded trays and
  can't pull them back in, so push it shut by hand. Eject still works.
