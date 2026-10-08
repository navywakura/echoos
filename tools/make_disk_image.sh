#!/usr/bin/env bash
# RXos — create an empty RXFS-formatted disk image ("RXF1" header, 0 files).
#
# v4.1: the in-OS ATA PIO driver reads/writes this image when it is attached
# as an IDE disk (make run-disk). Files `save`d in RXos survive reboots; the
# profile auto-restores at boot. Host-side: tools/inspect_image.py dumps it.
# Format spec: docs/rxfs_spec.md (matches fs/rxfs/rxfs_persist.c exactly).
set -euo pipefail
cd "$(dirname "$0")/.."
IMG="${1:-build/rxos-disk.img}"
SIZE_KB="${2:-512}"

mkdir -p "$(dirname "$IMG")"
python3 - "$IMG" "$SIZE_KB" <<'EOF'
import struct, sys
img, kb = sys.argv[1], int(sys.argv[2])
with open(img, "wb") as f:
    f.write(struct.pack("<IIII", 0x31465852, 1, 0, 0))  # RXF1 v1, 0 files
    f.truncate(kb * 1024)
print(f"[disk] wrote {img}: {kb} KiB, RXF1 header, 0 files")
EOF
echo "[disk] inspect it with:  python3 tools/inspect_image.py $IMG"
echo "[disk] boot with it:     make run-disk   (then in RXos: save / load)"
