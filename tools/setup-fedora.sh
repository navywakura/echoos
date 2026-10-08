#!/usr/bin/env bash
#
# Rogex OS — Fedora bare-metal toolchain bootstrap
# -------------------------------------------------
# Installs the low-level toolchain required to cross-compile a freestanding
# (no OS / no_std) x86_64 unikernel and package it into a bootable ISO.
#
# Target host: Fedora Linux (dnf).
# Usage:       ./scripts/setup-fedora.sh   (uses sudo for dnf)
#
set -euo pipefail

log()  { printf '\033[1;36m[rogex]\033[0m %s\n' "$*"; }
warn() { printf '\033[1;33m[rogex]\033[0m %s\n' "$*" >&2; }
die()  { printf '\033[1;31m[rogex]\033[0m %s\n' "$*" >&2; exit 1; }

[[ "$(uname -s)" == "Linux" ]] || die "This bootstrap targets Fedora Linux."
command -v dnf >/dev/null 2>&1 || die "dnf not found — this script is Fedora-specific."

SUDO=""
if [[ "${EUID}" -ne 0 ]]; then
    command -v sudo >/dev/null 2>&1 || die "Need root or sudo to install packages."
    SUDO="sudo"
fi

# ----------------------------------------------------------------------------
# 1. Critical low-level packages
# ----------------------------------------------------------------------------
#   llvm / clang      -> cross backend + freestanding C, lld linker
#   nasm              -> x86_64 assembler for the boot vector
#   gcc               -> alternate freestanding C backend
#   qemu-system-x86   -> emulate the bare-metal target for boot testing
#   mtools            -> build FAT images for the UEFI ESP without mounting
#   xorriso           -> author the El-Torito / hybrid bootable ISO
#   grub2 tools       -> grub-mkrescue (BIOS + UEFI hybrid boot) + EFI modules
PACKAGES=(
    llvm
    clang
    lld
    nasm
    gcc
    make
    qemu-system-x86
    mtools
    xorriso
    grub2-tools
    grub2-tools-extra
    grub2-efi-x64
    grub2-efi-x64-modules
    grub2-pc-modules
    binutils
)

log "Refreshing dnf metadata…"
${SUDO} dnf -y makecache

log "Installing low-level toolchain: ${PACKAGES[*]}"
${SUDO} dnf -y install "${PACKAGES[@]}"

# ----------------------------------------------------------------------------
# 2. Rust bare-metal toolchain (no_std / freestanding target)
# ----------------------------------------------------------------------------
if ! command -v rustup >/dev/null 2>&1; then
    log "Installing rustup (Rust toolchain manager)…"
    curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh -s -- -y
    # shellcheck disable=SC1090
    source "${HOME}/.cargo/env"
fi

log "Selecting stable toolchain and bare-metal components…"
rustup toolchain install stable
rustup default stable
# rust-src lets us rebuild core/alloc for exotic targets if ever needed.
rustup component add rust-src llvm-tools
# The freestanding, OS-less x86_64 target: no libc, no std, soft-float kernel.
rustup target add x86_64-unknown-none
# UEFI application target (used if you build a native UEFI stub bootloader).
rustup target add x86_64-unknown-uefi

# ----------------------------------------------------------------------------
# 3. Environment configuration for native cross-compilation
# ----------------------------------------------------------------------------
ENV_FILE="${ROGEX_ENV_FILE:-${HOME}/.rogex-env}"
log "Writing cross-compilation environment to ${ENV_FILE}"

cat > "${ENV_FILE}" <<'ENVEOF'
# ===========================================================================
#  Rogex OS — freestanding cross-compilation environment
#  Source this before building:   source ~/.rogex-env
# ===========================================================================

# --- Bare-metal C toolchain (freestanding: no host libc, no host runtime) ---
export ROGEX_TARGET="x86_64-unknown-none"
export CC="clang"
export LD="ld.lld"
export AS="nasm"
export AR="llvm-ar"
export OBJCOPY="llvm-objcopy"

# Freestanding compile flags: kernel code runs before any OS exists.
#   -ffreestanding    no hosted runtime assumptions
#   -fno-stack-protector / -fno-pic  no OS/loader services to rely on
#   -mno-red-zone     mandatory: interrupts must not clobber the red zone
#   -mno-*            disable SIMD until we save/restore that state ourselves
export ROGEX_CFLAGS="-target x86_64-none-elf -ffreestanding -nostdlib -nostdinc \
-fno-stack-protector -fno-stack-check -fno-pic -fno-pie \
-mno-red-zone -mno-mmx -mno-sse -mno-sse2 -mno-3dnow \
-mcmodel=kernel -std=c17 -Wall -Wextra -O2 -g"

# Assembler: NASM emitting 64-bit ELF objects.
export ROGEX_ASFLAGS="-f elf64 -g -F dwarf"

# Linker: use our custom script, produce a plain ELF (no host runtime).
export ROGEX_LDFLAGS="-nostdlib -static -z max-page-size=0x1000"

# --- Rust bare-metal (no_std) ---
export CARGO_BUILD_TARGET="x86_64-unknown-none"
# Kernel code model. The x86_64-unknown-none target already implies soft-float
# and disables SSE/MMX, matching the CPU state we set up in boot.asm.
export RUSTFLAGS="-C code-model=kernel -C relocation-model=static \
-C force-frame-pointers=yes"

# --- QEMU convenience: serial to stdio, no reboot on triple fault ---
export ROGEX_QEMU="qemu-system-x86_64 -machine q35 -m 512M -serial stdio \
-no-reboot -no-shutdown -d guest_errors"
ENVEOF

# ----------------------------------------------------------------------------
# 4. Verify
# ----------------------------------------------------------------------------
log "Verifying toolchain…"
missing=0
for tool in clang ld.lld nasm gcc qemu-system-x86_64 xorriso grub2-mkrescue mformat rustc cargo; do
    # grub-mkrescue is named grub2-mkrescue on Fedora
    real="${tool}"
    [[ "${tool}" == "grub2-mkrescue" ]] && real="grub2-mkrescue"
    if command -v "${real}" >/dev/null 2>&1; then
        printf '  \033[1;32m✓\033[0m %-20s %s\n' "${tool}" "$(command -v "${real}")"
    else
        printf '  \033[1;31m✗\033[0m %-20s MISSING\n' "${tool}"
        missing=1
    fi
done

echo
if [[ "${missing}" -eq 0 ]]; then
    log "Toolchain ready. Next:  source ${ENV_FILE} && make iso && make run"
else
    warn "Some tools are missing — re-run after resolving the dnf errors above."
fi
