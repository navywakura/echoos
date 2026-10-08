<p align="center"><img src="assets/brand/banner.png" width="100%" alt="ECHO OS 3.0" /></p>
<p align="center">
  <img src="https://img.shields.io/badge/echOS-3.0.0-111827" alt="echOS 3.0.0" />
  <img src="https://img.shields.io/badge/kernel-bare_metal-374151" alt="Bare metal kernel" />
  <img src="https://img.shields.io/badge/architectures-x86__64_%7C_AArch64-374151" alt="Architectures" />
  <img src="https://img.shields.io/badge/license-GPLv3-blue" alt="GPLv3" />
</p>
<p align="center">
  <img src="https://img.shields.io/badge/C-freestanding-A8B9CC?logo=c&logoColor=black" alt="C" />
  <img src="https://img.shields.io/badge/Rust-no__std-000000?logo=rust" alt="Rust" />
  <img src="https://img.shields.io/badge/Assembly-NASM-374151" alt="NASM" />
  <img src="https://img.shields.io/badge/LLVM-Clang_%2B_LLD-262D3A?logo=llvm" alt="LLVM" />
  <img src="https://img.shields.io/badge/QEMU-boot_tests-FF6600?logo=qemu" alt="QEMU" />
  <img src="https://img.shields.io/badge/Python-setup_%26_checks-3776AB?logo=python&logoColor=white" alt="Python" />
</p>

The complete **echOS 3.0 kernel source and build dependencies**, isolated from the retired chatbot, training experiments and websites. echOS is a robotic edge unikernel for x86_64 and AArch64: sensors produce bounded intents, a safety gate checks them, and accepted output can reach an autopilot through MAVLink.

The repository includes the C kernel, assembly boot code, Rust `no_std` library, hardware drivers, filesystem, required userland and build tools. Only images actually linked by the build, plus this banner, are retained.

## Verify and build

```bash
git clone https://github.com/navywakura/echoos.git
cd echoos
./setup.sh verify
./setup.sh                    # terminal setup and command launcher
make ARCH=x86_64 EDITION=universal -j2 all
make ARCH=x86_64 EDITION=universal iso
make ARCH=x86_64 EDITION=universal test
```

Python 3.11+ is required for setup. The setup verifies every distributed source file against its SHA-256 manifest and lists build/test commands. It detects tools without installing system packages or flashing disks. Build requirements: Clang, LLD, NASM, Cargo with the bare-metal Rust target, GNU Make, GRUB tools and xorriso. QEMU is used for boot tests; OVMF enables the UEFI test.

```bash
./setup.sh doctor
./setup.sh list
./setup.sh run build-isos
./setup.sh run portability
make build-universal-aarch64
```

## Release images

[echOS 3.0 Releases](https://github.com/navywakura/echoos/releases) provide the current universal VM and metal ISO images, complete source archive, SHA-256 checksums and build/boot evidence. The AArch64 boot artifact is an image rather than an ISO.

The universal VM and metal images use the same kernel and common universal GRUB configuration; they are separate download names, not distinct certified hardware profiles. QEMU boot evidence is not a claim of certification on arbitrary physical hardware. Inspect the evidence for the exact architecture and firmware tested.

```text
kernel/       Scheduling, memory, IPC, events, syscalls and robotics pipeline
arch/         x86_64 and AArch64 implementation
boot/         Boot assembly and GRUB configuration
drivers/      Platform, input, video, storage and networking
fs/           Filesystem and block interfaces
rogex-core/   Required Rust kernel library; no chatbot or website
userland/     Runtime, shell and embedded kernel services
ui/           Kernel-linked interface components
assets/       Build-linked resources and README banner
tools/        Build, setup and verification utilities
```

See [current release evidence](docs/RELEASE-3.0.0.md), [the historical 3.0 report](docs/ECHOS_3.0_REPORT.md) and [build guide](BUILDING.md). This repository contains no ECHO-AI runtime or language-model weights. Licensed under [GNU GPLv3](LICENSE), with component notices preserved.
