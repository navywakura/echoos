#!/usr/bin/env bash
#
# RXos — macOS toolchain (Apple Silicon / MacBook Air M1 is the reference).
#
# Installs QEMU so you can run the x86_64 VM ISO. Optionally installs the
# cross toolchain to *build* the kernel on the Mac.
#
#   tools/setup-macos.sh           # QEMU + checks (enough to run an ISO)
#   tools/setup-macos.sh --build   # also llvm/nasm/lld/rust for `make`
#
# Then:
#   tools/run_qemu_macos.sh        # or: make run-macos
#
set -euo pipefail

log()  { printf '\033[1;36m[rxos]\033[0m %s\n' "$*"; }
warn() { printf '\033[1;33m[rxos]\033[0m %s\n' "$*" >&2; }
die()  { printf '\033[1;31m[rxos]\033[0m %s\n' "$*" >&2; exit 1; }

[[ "$(uname -s)" == "Darwin" ]] || die "This script is for macOS (uname = $(uname -s))."

WANT_BUILD=0
for a in "$@"; do
    case "$a" in
        --build|-b) WANT_BUILD=1 ;;
        -h|--help)
            sed -n '2,16p' "$0"
            exit 0
            ;;
    esac
done

ARCH="$(uname -m)"
log "host: Darwin $ARCH ($(sysctl -n machdep.cpu.brand_string 2>/dev/null || echo CPU))"
if [[ "$ARCH" == "arm64" ]]; then
    log "Apple Silicon: QEMU will emulate x86_64 with TCG (HVF cannot run Intel guests)."
fi

# Homebrew
if ! command -v brew >/dev/null 2>&1; then
    die "Homebrew not found. Install from https://brew.sh  then re-run."
fi

# Apple Silicon brew lives in /opt/homebrew — make sure this shell sees it.
if [[ -x /opt/homebrew/bin/brew ]]; then
    eval "$(/opt/homebrew/bin/brew shellenv)"
elif [[ -x /usr/local/bin/brew ]]; then
    eval "$(/usr/local/bin/brew shellenv)"
fi

log "Installing qemu (qemu-system-x86_64)…"
brew list qemu >/dev/null 2>&1 || brew install qemu

if [[ "$WANT_BUILD" -eq 1 ]]; then
    log "Installing build tools (llvm, lld, nasm, xorriso, mtools, python)…"
    brew list llvm   >/dev/null 2>&1 || brew install llvm
    brew list lld    >/dev/null 2>&1 || brew install lld
    brew list nasm   >/dev/null 2>&1 || brew install nasm
    brew list xorriso >/dev/null 2>&1 || brew install xorriso
    brew list mtools >/dev/null 2>&1 || brew install mtools
    brew list python >/dev/null 2>&1 || brew install python

    # Point make at brew LLVM, not Apple clang (no x86_64-none-elf kernel model).
    LLVM_PREFIX="$(brew --prefix llvm)"
    LLD_PREFIX="$(brew --prefix lld 2>/dev/null || echo "$LLVM_PREFIX")"
    export PATH="$LLVM_PREFIX/bin:$LLD_PREFIX/bin:$PATH"

    if ! command -v rustup >/dev/null 2>&1 && ! command -v rustc >/dev/null 2>&1; then
        log "Installing rustup…"
        curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh -s -- -y
        # shellcheck disable=SC1091
        source "$HOME/.cargo/env"
    fi
    if command -v rustup >/dev/null 2>&1; then
        rustup toolchain install stable
        rustup target add x86_64-unknown-none
    fi

    warn "grub-mkrescue / i386-pc modules are often missing on Apple Silicon Homebrew."
    warn "ISO authoring (\`make iso\`) may fail here. Two working paths:"
    warn "  1) Copy build/rxOS-6.5.0-vm.iso from a Linux box (or CI) and run it."
    warn "  2) make run-elf     # Multiboot, no ISO, once the kernel ELF builds."
fi

ENV_FILE="${ROGEX_ENV_FILE:-$HOME/.rogex-env}"
{
    echo "# RXos — macOS ($(date +%Y-%m-%d))"
    echo "export QEMU=qemu-system-x86_64"
    if [[ "$WANT_BUILD" -eq 1 ]] && command -v brew >/dev/null 2>&1; then
        LP="$(brew --prefix llvm 2>/dev/null || true)"
        DP="$(brew --prefix lld 2>/dev/null || true)"
        if [[ -n "$LP" ]]; then
            echo "export PATH=\"$LP/bin:\${PATH}\""
            echo "export CC=\"$LP/bin/clang\""
        fi
        if [[ -n "$DP" && -x "$DP/bin/ld.lld" ]]; then
            echo "export LD=\"$DP/bin/ld.lld\""
        elif [[ -n "$LP" && -x "$LP/bin/ld.lld" ]]; then
            echo "export LD=\"$LP/bin/ld.lld\""
        fi
        echo "export AS=nasm"
    fi
} > "$ENV_FILE"
log "Wrote $ENV_FILE  —  source it in new shells:  source $ENV_FILE"

echo
log "QEMU: $(command -v qemu-system-x86_64 || echo MISSING)"
qemu-system-x86_64 --version 2>/dev/null | head -1 || true
echo
log "Next:"
echo "  source $ENV_FILE"
echo "  # ISO already built on Linux / this repo:"
echo "  tools/run_qemu_macos.sh build/rxOS-6.5.0-vm.iso"
echo "  # or: make run-macos"
echo
if [[ "$ARCH" == "arm64" ]]; then
    log "M1/M2/M3: first boot under TCG is slower than Fedora+KVM. Give it ~30s."
fi
