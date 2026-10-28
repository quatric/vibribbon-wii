#!/usr/bin/env python3
"""List or extract the *_FILES.PAK archives from a Vib-Ribbon disc image.

Usage: vibpak.py <Track 1.bin> list
       vibpak.py <Track 1.bin> extract <outdir>

Reads the MODE2/2352 data track directly. Nothing from the disc is stored in
this repository; extract into a directory that is git-ignored.
"""
import os, struct, sys

def sector(f, n):
    f.seek(n * 2352 + 24)
    return f.read(2048)

def read_file(f, lba, size):
    return b"".join(sector(f, lba + i) for i in range((size + 2047) // 2048))[:size]

def iso_walk(f, lba, ln, prefix=""):
    d = read_file(f, lba, ln)
    o = 0
    while o < ln:
        n = d[o]
        if n == 0:
            o = (o // 2048 + 1) * 2048
            continue
        el, es = struct.unpack("<I", d[o + 2:o + 6])[0], struct.unpack("<I", d[o + 10:o + 14])[0]
        name = d[o + 33:o + 33 + d[o + 32]].decode("latin1").split(";")[0]
        if name not in ("\x00", "\x01"):
            if d[o + 25] & 2:
                yield from iso_walk(f, el, es, prefix + name + "/")
            else:
                yield prefix + name, el, es
        o += n

def disc_files(f):
    pvd = sector(f, 16)
    assert pvd[1:6] == b"CD001", "not an ISO9660 MODE2/2352 track"
    root, rl = struct.unpack("<I", pvd[158:162])[0], struct.unpack("<I", pvd[166:170])[0]
    return {n: (l, s) for n, l, s in iso_walk(f, root, rl)}

def pak_entries(data):
    count = struct.unpack("<I", data[:4])[0]
    offs = struct.unpack("<%dI" % count, data[4:4 + 4 * count])
    for a in offs:
        name = data[a:a + 32].split(b"\0")[0]
        hdr = (len(name) + 4) & ~3          # name + NUL, padded to 4 bytes
        size = struct.unpack("<I", data[a + hdr:a + hdr + 4])[0]
        yield name.decode("latin1"), data[a + hdr + 4:a + hdr + 4 + size]

def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    with open(sys.argv[1], "rb") as f:
        files = disc_files(f)
        out = sys.argv[3] if sys.argv[2] == "extract" else None
        for path, (lba, size) in sorted(files.items()):
            if not (path.upper().endswith(("FILES.PAK", ".EXE", ".VH", ".VB"))
                    or path.upper().startswith("SCES_")):
                continue
            data = read_file(f, lba, size)
            if out:
                p = os.path.join(out, path); os.makedirs(os.path.dirname(p), exist_ok=True)
                open(p, "wb").write(data)
            if path.upper().endswith(".PAK"):
                for name, payload in pak_entries(data):
                    print(f"{path}: {name} {len(payload)}")
                    if out:
                        p = os.path.join(out, path + ".d", name); os.makedirs(os.path.dirname(p), exist_ok=True)
                        open(p, "wb").write(payload)

if __name__ == "__main__":
    main()
