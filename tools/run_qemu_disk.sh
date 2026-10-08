#!/usr/bin/env bash
# RXos — boot the ISO with a persistent disk attached + NIC (user/NAT).
#
# Default: q35 + virtio-blk (modern) -> disk "vda" + virtio-net.
# Set RXOS_DISK=ata for legacy path (machine pc + ATA PIO -> "hda").
# Disable net: RXOS_QEMU_NONET=1
set -euo pipefail
cd "$(dirname "$0")/.."
# shellcheck source=qemu_host.sh
. tools/qemu_host.sh
ISO="${1:-build/rxOS-7.0.0-vm.iso}"
IMG="${2:-build/rxos-disk.img}"
[[ -f "$ISO" ]] || { echo "ISO not found: $ISO  (run: make iso)"; exit 1; }
[[ -f "$IMG" ]] || { echo "disk image not found: $IMG  (run: make disk)"; exit 1; }

NET=( -netdev user,id=net0 -device virtio-net-pci,netdev=net0,mac=52:54:00:12:34:56 )
if [[ "${RXOS_QEMU_NONET:-}" == "1" ]]; then
  NET=()
fi

if [[ "${RXOS_DISK:-virtio}" == "ata" ]]; then
    # e1000 is the natural NIC on legacy pc/IDE (and VirtualBox default).
    ATA_NET=( -netdev user,id=net0 -device e1000,netdev=net0,mac=52:54:00:12:34:56 )
    if [[ "${RXOS_QEMU_NONET:-}" == "1" ]]; then
      ATA_NET=()
    fi
    exec "$QEMU" $QEMU_HOST_ARGS -machine pc -m 512M -cdrom "$ISO" \
         -drive file="$IMG",format=raw,if=ide \
         "${ATA_NET[@]}" \
         -serial stdio -no-reboot -no-shutdown
else
    exec "$QEMU" $QEMU_HOST_ARGS -machine q35 -m 512M -cdrom "$ISO" \
         -drive file="$IMG",format=raw,if=none,id=rxdisk0 \
         -device virtio-blk-pci,drive=rxdisk0 \
         "${NET[@]}" \
         -serial stdio -no-reboot -no-shutdown
fi
