#!/usr/bin/env bash
# RXos — build a bootable hard-disk image you can use without a USB stick.
#
# Strategy: the ISO produced by grub-mkrescue is already *isohybrid* — BIOS
# firmware treats it as a hard disk when you `dd` it to a drive or attach it
# as -hda in QEMU. We copy that hybrid image to build/rxos-hdd.img and pad
# it to a convenient size so it looks like a real disk.
#
# Install to an internal drive (DESTROYS the target):
#   make hdd
#   sudo dd if=build/rxos-hdd.img of=/dev/sdX bs=4M status=progress conv=fsync
#
# Test in QEMU (no CD-ROM):
#   make run-hdd
#
# After booting, use in-OS `partition` / `install` on a *second* disk (or a
# larger empty area you prepared) to keep RXFS files across reboots.
set -euo pipefail
cd "$(dirname "$0")/.."

IMG="${1:-build/rxos-hdd.img}"
SIZE_MB="${2:-256}"
ISO="${3:-build/rxOS-6.5.0-vm.iso}"

die() { echo "[hdd] error: $*" >&2; exit 1; }

[[ -f "$ISO" ]] || die "ISO not found: $ISO (run 'make iso' first)"

mkdir -p "$(dirname "$IMG")"
ISO_BYTES=$(stat -c%s "$ISO" 2>/dev/null || stat -f%z "$ISO")
ISO_MB=$(( (ISO_BYTES + 1024*1024 - 1) / (1024*1024) ))
if [[ "$SIZE_MB" -lt "$ISO_MB" ]]; then
  SIZE_MB=$ISO_MB
fi
TARGET_BYTES=$((SIZE_MB * 1024 * 1024))

echo "[hdd] hybrid ISO → hard-disk image"
echo "[hdd]   source: $ISO ($ISO_BYTES bytes)"
echo "[hdd]   target: $IMG (${SIZE_MB} MiB)"

# Start from a zeroed image, then lay the isohybrid in at LBA 0. Padding after
# the ISO is free space the user can reclaim offline if they want a data
# partition; the boot path does not depend on it.
dd if=/dev/zero of="$IMG" bs=1M count="$SIZE_MB" status=none
dd if="$ISO" of="$IMG" bs=1M conv=notrunc status=none

BYTES=$(stat -c%s "$IMG" 2>/dev/null || stat -f%z "$IMG")
echo "[hdd] built $IMG (${BYTES} bytes)"
echo "[hdd] QEMU:    make run-hdd"
echo "[hdd] real disk (DESTROYS TARGET — check lsblk first):"
echo "        sudo dd if=$IMG of=/dev/sdX bs=4M status=progress conv=fsync"
echo "[hdd] (same result as dd'ing the ISO; the .img is just padded)"
