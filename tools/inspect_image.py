#!/usr/bin/env python3
"""RXos — inspect an RXFS "RXF1" image (host side).

Layout (little-endian; must match fs/rxfs/rxfs.h + rxfs_persist.c):
  sector 0:            rxfs_img_header { magic u32, version u32, count u32, rsvd u32 }
  then per file, sector-aligned records of 9 sectors (4148 B -> 4608 B):
                       rxfs_img_file { name[48], len u32, data[4096] }

Usage:  python3 tools/inspect_image.py build/rxos-disk.img [--cat NAME]
"""
import struct, sys

SECTOR = 512
NAME = 48
FSIZE = 4096
REC = NAME + 4 + FSIZE                 # 4148
REC_SECTORS = (REC + SECTOR - 1) // SECTOR

def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    path = sys.argv[1]
    cat = sys.argv[3] if len(sys.argv) > 3 and sys.argv[2] == "--cat" else None

    with open(path, "rb") as f:
        blob = f.read()
    if len(blob) < 16:
        sys.exit("image too small")
    magic, version, count, _ = struct.unpack_from("<IIII", blob, 0)
    if magic != 0x31465852:
        sys.exit(f"bad magic 0x{magic:08x} (want RXF1 = 0x31465852)")
    print(f"RXFS image: {path}")
    print(f"  version {version}, {count} file(s), image {len(blob)} bytes")
    off = SECTOR
    for i in range(count):
        raw = blob[off : off + REC]
        name = raw[:NAME].split(b"\0")[0].decode(errors="replace")
        (length,) = struct.unpack_from("<I", raw, NAME)
        data = raw[NAME + 4 : NAME + 4 + min(length, FSIZE)]
        print(f"  [{i}] {name:<26} {length} B")
        if cat and name == cat:
            print("-" * 40)
            sys.stdout.write(data.decode(errors="replace"))
            print("-" * 40)
        off += REC_SECTORS * SECTOR

if __name__ == "__main__":
    main()
