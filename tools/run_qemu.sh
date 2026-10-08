#!/usr/bin/env bash
# RXos — boot the ISO in QEMU (graphical window + serial on stdio + NIC).
#
# Default NIC: virtio-net on QEMU user/NAT (host internet → guest).
# Inside the OS: `www on` then `www status` / `rx app add …`
# Disable: RXOS_QEMU_NONET=1 tools/run_qemu.sh
set -euo pipefail
cd "$(dirname "$0")/.."
# shellcheck source=qemu_host.sh
. tools/qemu_host.sh
ISO="${1:-build/rxOS-7.0.0-vm.iso}"
[[ -f "$ISO" ]] || { echo "ISO not found: $ISO  (run: make iso)"; exit 1; }

NIC="${RXOS_QEMU_NIC:-virtio}"
if [[ "$NIC" == "e1000" ]]; then
  NET=( -netdev user,id=net0 -device e1000,netdev=net0,bus=pcie.0,mac=52:54:00:12:34:56 )
else
  NET=( -netdev user,id=net0 -device virtio-net-pci,netdev=net0,bus=pcie.0,mac=52:54:00:12:34:56 )
fi
if [[ "${RXOS_QEMU_NONET:-}" == "1" ]]; then
  NET=()
fi

[[ -n "$QEMU_HOST_NOTE" ]] && echo "[rxos] $QEMU_HOST_NOTE"

exec "$QEMU" $QEMU_HOST_ARGS -machine q35 -m 512M -cdrom "$ISO" \
     -serial stdio -no-reboot -no-shutdown \
     "${NET[@]}" \
     "${@:2}"
