# Host-specific QEMU extras. Source from the other run scripts.
# Linux: leave accel to QEMU (KVM when present).
# macOS / Apple Silicon: x86_64 guests cannot use HVF — force TCG.
#
# Usage (after cd to repo root is fine, not required):
#   . tools/qemu_host.sh
#   "$QEMU" $QEMU_HOST_ARGS -machine q35 ...
#
# QEMU_HOST_ARGS is a string (not an array) so macOS /bin/bash 3.2 + set -u
# does not die on an empty expansion.

QEMU="${QEMU:-qemu-system-x86_64}"
QEMU_HOST_ARGS=""
QEMU_HOST_NOTE=""

_rxos_uname_s="$(uname -s 2>/dev/null || echo unknown)"
_rxos_uname_m="$(uname -m 2>/dev/null || echo unknown)"

if [[ "$_rxos_uname_s" == "Darwin" ]]; then
    # HVF on Apple Silicon only accelerates aarch64 guests. An x86_64 RXos
    # guest must go through TCG. Multi-thread TCG + qemu64 (no host CPUID
    # passthrough) is the combination that actually boots on an M1/M2/M3 Air.
    QEMU_HOST_ARGS="-accel tcg,thread=multi -cpu qemu64 -smp 2"
    if [[ "$_rxos_uname_m" == "arm64" ]]; then
        QEMU_HOST_NOTE="macOS Apple Silicon ($_rxos_uname_m): x86_64 via TCG (no HVF)"
    else
        QEMU_HOST_NOTE="macOS Intel: TCG (HVF x86 is optional; TCG is the safe default)"
    fi
fi
