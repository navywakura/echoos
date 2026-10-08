#!/usr/bin/env bash
# RXos — verify the build/run toolchain is present.
# Fedora: tools/setup-fedora.sh
# macOS (M1 Air): tools/setup-macos.sh [--build]
set -u

ok=0; missing=0

check() {  # check <binary> [alt-binary] <what-for>
    local name="$1" alt="${2:-}" why="${3:-$2}"
    if command -v "$name" >/dev/null 2>&1; then
        printf '  [ok]      %-22s %s\n' "$name" "$(command -v "$name")"
        ok=$((ok+1))
    elif [[ -n "$alt" ]] && command -v "$alt" >/dev/null 2>&1; then
        printf '  [ok]      %-22s %s (as %s)\n' "$name" "$(command -v "$alt")" "$alt"
        ok=$((ok+1))
    else
        printf '  [MISSING] %-22s needed for: %s\n' "$name" "$why"
        missing=$((missing+1))
    fi
}

echo "RXos v4 Foundation - toolchain check"
echo
check clang gcc          "compiling the freestanding C kernel"
check nasm ""            "assembling boot/boot.asm + arch/x86_64/isr.asm"
check ld.lld ld          "linking the kernel ELF"
check cargo ""           "building rogex-core (Rust no_std)"
check grub-mkrescue grub2-mkrescue "authoring the bootable ISO"
check xorriso ""         "ISO 9660 mastering (used by grub-mkrescue)"
check qemu-system-x86_64 "" "running RXos in a VM"
check make gmake         "driving the build"
check python3 ""         "tools/mkapps.py, tools/rgxpack.py, tools/inspect_image.py"
echo
if [[ $missing -eq 0 ]]; then
    echo "all $ok tools present - you can:  make iso && make run"
    [[ "$(uname -s)" == "Darwin" ]] && echo "macOS:  make run-macos   (Apple Silicon uses TCG)"
else
    echo "$missing tool(s) missing."
    echo "  Fedora:  tools/setup-fedora.sh"
    echo "  macOS:   tools/setup-macos.sh --build     # QEMU-only: omit --build"
    echo "           Apple Silicon Homebrew often lacks grub-mkrescue (i386-pc)."
    echo "           Copy build/rxOS-6.5.0-vm.iso from Linux, then: make run-macos"
    exit 1
fi
