# Building echOS 3.0

Run `./setup.sh verify` before building. `./setup.sh doctor` reports missing tools; installation of system packages remains explicit.

Fedora reference tools: clang, llvm, lld, nasm, make, cargo, rust, grub2-tools-extra, xorriso and qemu-system-x86. Install the `x86_64-unknown-none` Rust target using the toolchain's target manager. For AArch64 use `aarch64-unknown-none`.

```bash
make ARCH=x86_64 EDITION=universal -j2 all
make ARCH=x86_64 EDITION=universal iso
make ARCH=x86_64 EDITION=universal test
make ARCH=x86_64 EDITION=universal test-uefi
make build-universal-aarch64
```

Artifacts are written under `build/ARCH/EDITION/`. Kernel code is freestanding C/assembly plus the required Rust `no_std` library. GRUB and xorriso create the VM and metal ISO images. AArch64 produces `rxos.img`.

The source manifest excludes generated build outputs and local configuration. Editing source invalidates the pristine-source check; the setup accepts `--allow-modified` for intentional development. Build commands do not flash host disks.
