#!/usr/bin/env bash
# RXos on macOS — QEMU x86_64. Written for Apple Silicon (MacBook Air M1).
#
# The guest is still Intel RXos. On M1/M2/M3, QEMU emulates that CPU in
# software (TCG). Do not pass -accel hvf: HVF only runs ARM guests.
#
#   tools/setup-macos.sh          # once: brew qemu (+ optional toolchain)
#   make iso-vm                   # or copy build/rxOS-6.5.0-vm.iso from Linux
#   tools/run_qemu_macos.sh       # cocoa window + serial in the terminal
#   make run-macos                # same
#
set -euo pipefail
cd "$(dirname "$0")/.."
# shellcheck source=qemu_host.sh
. tools/qemu_host.sh

ISO="${1:-build/rxOS-6.5.0-vm.iso}"
[[ -f "$ISO" ]] || {
    echo "ISO not found: $ISO"
    echo "  On this Mac:        make iso-vm     (needs grub-mkrescue — often missing)"
    echo "  Or copy from Linux: build/rxOS-6.5.0-vm.iso"
    echo "  ELF-only (no ISO):  make run-elf"
    exit 1
}

if ! command -v "$QEMU" >/dev/null 2>&1; then
    echo "qemu-system-x86_64 not in PATH."
    echo "  brew install qemu"
    echo "  echo 'eval \"\$(/opt/homebrew/bin/brew shellenv)\"' >> ~/.zprofile"
    exit 1
fi

NET=( -netdev user,id=net0 -device virtio-net-pci,netdev=net0,mac=52:54:00:12:34:56 )
if [[ "${RXOS_QEMU_NONET:-}" == "1" ]]; then
    NET=()
fi

echo "[rxos] $QEMU_HOST_NOTE"
echo "[rxos] ISO $ISO"
echo "[rxos] virtio-net user/NAT — inside OS: www on"
echo "[rxos] cocoa window = display; this terminal = serial"

exec "$QEMU" \
    $QEMU_HOST_ARGS \
    -machine q35 \
    -m "${RXOS_QEMU_MEM:-1G}" \
    -display cocoa \
    -vga std \
    -cdrom "$ISO" \
    -serial stdio \
    -no-reboot -no-shutdown \
    "${NET[@]}" \
    "${@:2}"
