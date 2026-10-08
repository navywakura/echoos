"""Pin the one field grub-mkrescue stamps with the wall clock.

GRUB 2.12 as shipped here writes a marker file /.disk/<build-time>.uuid and
embeds `search --set=root --file /.disk/<build-time>.uuid` in every core
image it builds. That timestamp comes from time(NULL); this build honours
neither SOURCE_DATE_EPOCH nor any command-line date, so two identical trees
produce two different ISOs and no hash can be compared.

The marker is plain ASCII in the core images and in the Rock Ridge / ISO9660
names, and UTF-16BE in the Joliet tree. Nothing compresses it, nothing sums
over it, and the replacement has the same length, so rewriting the stamp in
place leaves a valid image whose `search` still names a file that exists.

The embedded EFI System Partition is a FAT image whose 4-byte volume serial
(BS_VolID) is seeded from the clock the same way. It is informational here —
nothing in the boot path looks it up — so it is pinned as well.

This normalises one known field. It is not a reproducibility certificate:
that is two clean builds hashing the same, and the booted image still
passing tools/smoke_universal.py.
"""
import datetime
import re
import sys

MARKER = re.compile(rb"(\d{4}(?:-\d{2}){6})(\.uuid)", re.IGNORECASE)
# BS_BootSig 0x29, BS_VolID (4), BS_VolLab (11), BS_FilSysType (8).
FAT_VOLID = re.compile(rb"\x29(....)([\x20-\x7e]{11})(FAT1[26]   |FAT     )", re.S)


def utf16be(raw):
    return raw.decode("ascii").encode("utf-16-be")


def replace_all(data, old, new):
    """Rewrite every occurrence of old, in place, same length. Returns count."""
    if len(old) != len(new):
        raise SystemExit(f"refusing to resize {old!r} -> {new!r}")
    count, at = 0, data.find(old)
    while at >= 0:
        data[at:at + len(old)] = new
        count += 1
        at = data.find(old, at + len(new))
    return count


def pin_fat_serial(data, epoch):
    """Pin BS_VolID in every embedded FAT boot sector. Returns count."""
    want = (epoch & 0xFFFFFFFF).to_bytes(4, "little")
    hits = 0
    for m in list(FAT_VOLID.finditer(bytes(data))):
        if data[m.start(1):m.end(1)] == want:
            continue
        data[m.start(1):m.end(1)] = want
        hits += 1
    return hits


def main():
    path, epoch = sys.argv[1], int(sys.argv[2])
    fixed = datetime.datetime.fromtimestamp(
        epoch, datetime.timezone.utc).strftime("%Y-%m-%d-%H-%M-%S-00").encode()

    data = bytearray(open(path, "rb").read())
    before = len(data)

    fat = pin_fat_serial(data, epoch)

    stamps = {m.group(1) for m in MARKER.finditer(bytes(data))}
    if not stamps:
        raise SystemExit(f"{path}: no GRUB .uuid marker found; refusing to guess")
    if len(stamps) != 1:
        raise SystemExit(f"{path}: {len(stamps)} distinct markers: {stamps!r}")
    stamp = stamps.pop()
    if len(stamp) != len(fixed):
        raise SystemExit(f"{path}: marker {stamp!r} is not {len(fixed)} bytes")
    if stamp == fixed:
        if fat:
            open(path, "wb").write(bytes(data))
        print(f"[iso] uuid marker already {fixed.decode()}; {fat} FAT serials pinned")
        return

    # Replace the marker with its extension attached, so a coincidental run of
    # the same digits elsewhere in the image is left alone.
    hits = 0
    for ext in (b".uuid", b".UUID"):
        hits += replace_all(data, stamp + ext, fixed + ext)
        hits += replace_all(data, utf16be(stamp + ext), utf16be(fixed + ext))
    if not hits:
        raise SystemExit(f"{path}: marker matched but no span replaced")
    if len(data) != before:
        raise SystemExit(f"{path}: length changed {before} -> {len(data)}")
    if MARKER.search(bytes(data)) and stamp in bytes(data):
        raise SystemExit(f"{path}: build-time stamp {stamp.decode()} still present")

    open(path, "wb").write(bytes(data))
    print(f"[iso] pinned GRUB uuid marker {stamp.decode()} -> {fixed.decode()} "
          f"({hits} spans, {fat} FAT serials)")


if __name__ == "__main__":
    main()
