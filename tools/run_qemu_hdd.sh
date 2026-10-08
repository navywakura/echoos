#!/usr/bin/env bash
# Boot the RXos hard-disk image in QEMU (no CD-ROM / no USB).
set -euo pipefail
cd "$(dirname "$0")/.."

# shellcheck source=qemu_host.sh
. tools/qemu_host.sh

IMG="${1:-build/rxos-hdd.img}"

[[ -f "$IMG" ]] || { echo "missing $IMG — run: make hdd"; exit 1; }

# IDE/legacy pc machine: SeaBIOS boots isohybrid MBR from a plain hard disk.
# virtio as the *boot* disk is unreliable under BIOS without extra firmware.
# e1000 + user/NAT so `www on` works the same as make run.
NET=( -netdev user,id=net0 -device e1000,netdev=net0,mac=52:54:00:12:34:56 )
if [[ "${RXOS_QEMU_NONET:-}" == "1" ]]; then
  NET=()
fi

exec "$QEMU" \
  $QEMU_HOST_ARGS \
  -machine pc \
  -m 512M \
  -serial stdio \
  -no-reboot -no-shutdown \
  -drive "file=${IMG},format=raw,if=ide" \
  -boot order=c \
  "${NET[@]}"
