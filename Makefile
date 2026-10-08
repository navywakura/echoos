# ============================================================================

.DEFAULT_GOAL := all
# ISO targets stage into one directory; never stage two menus concurrently.
.NOTPARALLEL: iso iso-refresh edition-universal edition-server edition-complete edition-minimal edition-edge edition-dev
#  echOS 2.1.0-honest — build orchestration
#  ASM (nasm) + C (freestanding clang) + Rust (no_std cargo)  ->  ELF  ->  ISO
#
#  Usage:
#    make              # build the kernel ELF
#    make iso          # build BOTH ISOs (vm + metal)
#    make iso-vm       # QEMU / VirtualBox ISO
#    make iso-metal    # real PC / USB ISO (hybrid-friendly)
#    make run          # boot VM ISO in QEMU
#    make test         # automated QEMU smoke test
#    make clean
# ============================================================================

# --- tools ------------------------------------------------------------------
# NOTE: CC/AS/LD have built-in make defaults (cc/as/ld), so `?=` will NOT
# override them. Only replace them when the user hasn't set them explicitly.
ifeq ($(origin CC),default)
  CC := clang
endif
ifeq ($(origin AS),default)
  AS := nasm
endif
ifeq ($(origin LD),default)
  LD := ld.lld
endif
CARGO   ?= cargo
QEMU    ?= qemu-system-x86_64
SOURCE_DATE_EPOCH ?= 1704067200
export SOURCE_DATE_EPOCH
ISO_DATE := $(shell date -u -d @$(SOURCE_DATE_EPOCH) +%Y%m%d%H%M%S00)

# Apple Silicon cannot HVF-accelerate an x86_64 guest — TCG only.
# Linux keeps the default (KVM when the host can). See docs/macos-m1.md.
UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Darwin)
  QEMU_HOST := -accel tcg,thread=multi -cpu qemu64 -smp 2
else
  QEMU_HOST :=
endif

# grub-mkrescue is `grub2-mkrescue` on Fedora.
GRUB_MKRESCUE := $(shell command -v grub-mkrescue 2>/dev/null || command -v grub2-mkrescue 2>/dev/null)

# --- layout -----------------------------------------------------------------
ARCH       ?= x86_64
EDITION    ?= universal
BUILD_ROOT ?= build
# BUILD is derived, never a shared directory supplied by a caller. Use
# BUILD_ROOT=/tmp/my-build for an independent tree.
override BUILD := $(BUILD_ROOT)/$(ARCH)/$(EDITION)
ifeq ($(filter $(ARCH),x86_64 aarch64),)
  $(error Unsupported ARCH=$(ARCH); this tree targets x86_64 and aarch64)
endif
ifeq ($(filter $(EDITION),universal server minimal complete edge dev),)
  $(error Unsupported EDITION=$(EDITION))
endif
ISODIR     := $(BUILD)/isodir
KERNEL_ELF := $(BUILD)/rxos.elf
KERNEL_BIN := $(BUILD)/rxos.bin
# --- editions ----------------------------------------------------------------
# make edition-complete / edition-minimal / edition-edge / edition-universal
ifeq ($(EDITION),universal)
  ECHOS_VER := 3.0.0
else ifeq ($(EDITION),server)
  ECHOS_VER := 2.1.0
else
  ECHOS_VER := 1.0.0
endif
ECHOS_TAG := $(ECHOS_VER)-$(EDITION)
ECHOS_EDITION_UP := $(shell echo $(EDITION) | tr a-z A-Z)
ifeq ($(EDITION),universal)
  GRUB_VM_SRC    := boot/grub/grub-universal.cfg
  GRUB_METAL_SRC := boot/grub/grub-universal.cfg
else
  GRUB_VM_SRC    := boot/grub/grub-vm.cfg
  GRUB_METAL_SRC := boot/grub/grub-metal.cfg
endif

ISO_VM     := $(BUILD)/EchOS-$(ECHOS_TAG)-vm.iso
ISO_METAL  := $(BUILD)/EchOS-$(ECHOS_TAG)-metal.iso
OBJCOPY    := $(shell command -v llvm-objcopy 2>/dev/null || command -v objcopy 2>/dev/null)
ISO        := $(ISO_VM)
DISK_IMG   := $(BUILD)/rxos-disk.img
HDD_IMG    := $(BUILD)/rxos-hdd.img

ifeq ($(ARCH),x86_64)
  RUST_TARGET := x86_64-unknown-none
else
  RUST_TARGET := aarch64-unknown-none
endif
RUST_BUILD  := $(abspath $(BUILD))/rust
RUST_LIB    := $(RUST_BUILD)/$(RUST_TARGET)/release/librogex_core.a

# --- sources ----------------------------------------------------------------
# Trees every target compiles: after phase F these name no machine.
C_DIRS_GENERIC := kernel kernel/boot kernel/robot kernel/memory kernel/task \
          kernel/syscall kernel/ipc kernel/event \
          fs fs/rxfs \
          userland/init userland/shell userland/rosh userland/apps \
          userland/runtime userland/cli

# Trees that are allowed to know what machine this is.
ifeq ($(ARCH),x86_64)
  C_DIRS := $(C_DIRS_GENERIC) arch arch/x86_64 \
          drivers drivers/platform drivers/video drivers/input drivers/timer \
          drivers/storage drivers/pci drivers/net drivers/npu \
          kernel/gui ui
  ASM_SRC := boot/boot.asm boot/wallpaper.asm boot/iconpack.asm boot/hdboot_blob.asm \
           arch/x86_64/isr.asm arch/x86_64/switch.asm
  ASM_OBJ := $(patsubst %.asm,$(BUILD)/%.o,$(ASM_SRC))
else
  # AArch64, first boot: the kernel core, not the 2.1 userland. The desktop
  # userland is tied to the PC driver set (framebuffer, PS/2, PCI, TLS,
  # window server); pulling it in would mean 200 stubs pretending to be
  # devices. This target builds what the phase's checklist actually asks for
  # and says out loud what it does not have.
  C_DIRS := kernel kernel/boot kernel/robot kernel/memory kernel/task \
            kernel/syscall kernel/ipc kernel/event fs fs/rxfs \
            arch arch/aarch64 drivers/aarch64
  # kernel/kernel.c is the PC board's main; aarch64 has its own.
  # kernel/hwid.c reads a TPM at a fixed x86 address.
  C_EXCLUDE := kernel/kernel.c kernel/hwid.c
  # Generic drivers with no machine in them, taken by name rather than by
  # directory so nothing x86 comes along for the ride.
  # netif_pci.c stays behind: this board enumerates virtio-mmio, not a bus.
  C_EXTRA := drivers/storage/ramdisk.c drivers/storage/blockdev.c \
             drivers/net/netif.c drivers/net/ipstack.c
  ASM_SRC :=
  ASM_OBJ := $(BUILD)/arch/aarch64/boot.o $(BUILD)/arch/aarch64/switch.o \
             $(BUILD)/arch/aarch64/vectors.o $(BUILD)/arch/aarch64/efi_stub.o
endif

C_SRC   := $(filter-out $(C_EXCLUDE),$(foreach d,$(C_DIRS),$(wildcard $(d)/*.c))) $(C_EXTRA)
C_OBJ   := $(patsubst %.c,$(BUILD)/%.o,$(C_SRC))
INCLUDES := $(addprefix -I,$(C_DIRS)) -Ikernel/include -Idrivers -Idrivers/video \
            -Idrivers/input -Idrivers/timer -Idrivers/storage -Idrivers/net \
            -Idrivers/platform -Idrivers/pci -Idrivers/npu -Iui -Ikernel/gui

# --- flags ------------------------------------------------------------------
# Freestanding: no host libc/runtime, no red zone, no SIMD until we manage it.
AERO ?= 1
CFLAGS_COMMON := -ffreestanding -nostdlib -nostdinc \
          -fno-stack-protector -fno-stack-check -fno-pic -fno-pie \
          -std=c17 -Wall -Wextra -O2 -g $(INCLUDES) \
          -fdebug-prefix-map=$(abspath .)=. \
          -DECHOS_EDITION_$(shell echo $(EDITION) | tr a-z A-Z)=1 \
          -MMD -MP

ifeq ($(ARCH),x86_64)
  CFLAGS := -target x86_64-none-elf -mno-red-zone -mno-mmx -mno-sse -mno-sse2 \
            -mcmodel=kernel -DRXOS_AERO=$(AERO) $(CFLAGS_COMMON)
  ASFLAGS := -f elf64 -g -F dwarf
  LDFLAGS := -n -nostdlib -static --gc-sections -T linker.ld
else
  # -mgeneral-regs-only: no FP/SIMD in the kernel, so an exception handler
  # never has to save a vector register bank it does not own.
  CFLAGS := -target aarch64-none-elf -mgeneral-regs-only -mcmodel=small \
            -DRXOS_AERO=0 $(CFLAGS_COMMON)
  ASFLAGS :=
  LDFLAGS := -nostdlib -static --gc-sections -T linker-aarch64.ld
  LDSCRIPT := linker-aarch64.ld
endif
LDSCRIPT ?= linker.ld
# The bootable artefact on aarch64 is the flat image: only that carries the
# arm64 boot header a loader reads to place it and pass the device tree.
KERNEL_IMG := $(BUILD)/rxos.img

.PHONY: all iso iso-vm iso-metal iso-refresh stage-epk-media capture run run-macos run-e1000 run-shell run-desktop run-fs run-elf disk run-disk \
        hdd run-hdd \
        test test-disk net-test net-test-e1000 net-test-host qoi-test apps site fuzz check clean rust-core dirs \

all: $(KERNEL_ELF)

.PHONY: build-universal-x86_64 test-universal-x86_64 test-uefi-universal-x86_64 \
        run-universal-x86_64 run-uefi-universal-x86_64 repro-universal-x86_64 FORCE
build-universal-x86_64:
	$(MAKE) ARCH=x86_64 EDITION=universal iso-vm
test-universal-x86_64:
	$(MAKE) ARCH=x86_64 EDITION=universal test
test-uefi-universal-x86_64:
	$(MAKE) ARCH=x86_64 EDITION=universal test-uefi
run-universal-x86_64:
	$(MAKE) ARCH=x86_64 EDITION=universal run
run-uefi-universal-x86_64:
	$(MAKE) ARCH=x86_64 EDITION=universal run-uefi

# CERT-21: two clean builds in independent trees must hash the same. Runs the
# whole pipeline twice from nothing, so it is slow on purpose — a warm tree
# proves nothing about reproducibility.
REPRO_A := $(BUILD_ROOT)/repro/a/x86_64/universal
REPRO_B := $(BUILD_ROOT)/repro/b/x86_64/universal
repro-universal-x86_64:
	@rm -rf $(BUILD_ROOT)/repro
	$(MAKE) BUILD_ROOT=$(BUILD_ROOT)/repro/a ARCH=x86_64 EDITION=universal iso-vm
	$(MAKE) BUILD_ROOT=$(BUILD_ROOT)/repro/b ARCH=x86_64 EDITION=universal iso-vm
	@fail=0; for f in rxos.elf rxos.bin EchOS-$(ECHOS_TAG)-vm.iso; do \
	    a=$$(sha256sum "$(REPRO_A)/$$f" | cut -d" " -f1); \
	    b=$$(sha256sum "$(REPRO_B)/$$f" | cut -d" " -f1); \
	    if [ "$$a" = "$$b" ]; then echo "[repro] OK   $$f  $$a"; \
	    else echo "[repro] DIFF $$f"; echo "          a=$$a"; echo "          b=$$b"; fail=1; fi; \
	done; exit $$fail

# Rebuild on flag/tool changes within an edition as well as header changes.
# An unchanged configuration keeps its mtime, so incremental builds stay cheap.
FORCE:
$(BUILD)/config.txt: FORCE | dirs
	@python3 tools/build_config.py "$@" "$(CC)" "$(AS)" "$(LD)" "$(CFLAGS)" "$(ASFLAGS)" "$(LDFLAGS)"
$(C_OBJ) $(ASM_OBJ): $(BUILD)/config.txt

dirs:
	@mkdir -p $(BUILD)

# --- assembly ---------------------------------------------------------------
$(BUILD)/%.o: %.asm | dirs
	@mkdir -p $(@D)
	$(AS) $(ASFLAGS) $< -o $@

# --- C ----------------------------------------------------------------------
# Header dependency tracking. Without it, editing a header never rebuilt the
# .c files that include it: a struct could gain a field and half the objects
# would keep the old layout, producing a kernel that corrupts itself at
# runtime with no build error. That is exactly what happened when rx_actor_t
# gained a field — a #GP with a garbage rip, from stale objects.
-include $(C_OBJ:.o=.d)

$(BUILD)/%.o: %.c | dirs
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -c $< -o $@

# incbin'd asset: nasm has no dependency output, so state it.
$(BUILD)/hdboot.bin: boot/hdboot.asm boot/hdboot_s2.asm | dirs
	$(AS) -f bin boot/hdboot.asm -o $(BUILD)/hdboot_s1.bin
	$(AS) -f bin boot/hdboot_s2.asm -o $(BUILD)/hdboot_s2.bin
	@s1=$$(wc -c < $(BUILD)/hdboot_s1.bin); s2=$$(wc -c < $(BUILD)/hdboot_s2.bin); \
	 echo "[rxos] hdboot stage1 $$s1 stage2 $$s2"; \
	 test $$s1 -eq 512 || { echo "MBR must be 512 bytes"; exit 1; }; \
	 test $$s2 -le $$((16 * 512)) || { echo "stage2 too big"; exit 1; }
	cat $(BUILD)/hdboot_s1.bin $(BUILD)/hdboot_s2.bin > $@

$(BUILD)/arch/aarch64/%.o: arch/aarch64/%.S | dirs
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/boot/hdboot_blob.o: boot/hdboot_blob.asm $(BUILD)/hdboot.bin | dirs
	@mkdir -p $(@D)
	$(AS) $(ASFLAGS) -DHDBOOT_BIN='"$(abspath $(BUILD)/hdboot.bin)"' $< -o $@

$(BUILD)/boot/wallpaper.o: boot/wallpaper.asm assets/wallpaper/pack/gauges.jpg \
	assets/wallpaper/pack/steel.jpg assets/wallpaper/pack/wood.jpg \
	assets/wallpaper/pack/mountain.jpg assets/wallpaper/pack/lake.jpg \
	assets/wallpaper/pack/night.jpg assets/wallpaper/pack/forest.jpg \
	assets/wallpaper/pack/sea.jpg assets/wallpaper/pack/fog.jpg \
	assets/wallpaper/pack/dune.jpg assets/wallpaper/pack/portrait.jpg \
	assets/wallpaper/pack/meadow.jpg
$(BUILD)/boot/iconpack.o: boot/iconpack.asm assets/icons/pack/ajustes.rgba

# Generated-header dependencies (no automatic header tracking).
$(BUILD)/drivers/video/font.o: drivers/video/font_jbmono.h
$(BUILD)/userland/runtime/rgxsite.o:  userland/runtime/site_social.h
$(BUILD)/userland/init/init.o:        userland/apps/apps.h

# --- Rust no_std core -------------------------------------------------------
rust-core:
	cd rogex-core && $(CARGO) build --release --locked --target $(RUST_TARGET) --target-dir "$(RUST_BUILD)"
$(RUST_LIB): rust-core

# --- link -------------------------------------------------------------------
$(KERNEL_ELF): $(ASM_OBJ) $(C_OBJ) $(RUST_LIB) $(LDSCRIPT)
	$(LD) $(LDFLAGS) -o $@ $(ASM_OBJ) $(C_OBJ) $(RUST_LIB)
	@echo "[rxos] linked $@"

$(KERNEL_IMG): $(KERNEL_ELF)
	@test -n "$(OBJCOPY)" || { echo "llvm-objcopy / objcopy not found"; exit 1; }
	$(OBJCOPY) -O binary -S $< $@
	@echo "[rxos] image $@"

$(KERNEL_BIN): $(KERNEL_ELF)
	@test -n "$(OBJCOPY)" || { echo "llvm-objcopy / objcopy not found"; exit 1; }
	$(OBJCOPY) -O binary -S $< $@
	@echo "[rxos] payload $@"

# Remake the ISOs with the current .bin. No clang, no rust.
iso-refresh: $(KERNEL_ELF) $(KERNEL_BIN)
	$(MAKE) $(ISO_VM) $(ISO_METAL)

# --- bootable ISOs (GRUB, BIOS boot; UEFI-native is roadmap) -----------------
# Same kernel; two GRUB menus: VM-friendly vs real-hardware VESA list.
iso: iso-vm iso-metal
	@echo "[rxos] ISOs: $(ISO_VM)  $(ISO_METAL)"

iso-vm: $(ISO_VM)
iso-metal: $(ISO_METAL)

# --- EchOS editions ----------------------------------------------------------
# Same kernel tree, three product lines. Example: make edition-complete
edition-complete:
	@$(MAKE) --no-print-directory EDITION=complete iso
edition-minimal:
	@$(MAKE) --no-print-directory EDITION=minimal iso
edition-edge:
	@$(MAKE) --no-print-directory EDITION=edge iso
edition-dev:
	@$(MAKE) --no-print-directory EDITION=dev iso
edition-universal:
	@$(MAKE) --no-print-directory EDITION=universal iso
edition-server:
	@$(MAKE) --no-print-directory EDITION=server iso
editions: edition-complete edition-minimal edition-edge edition-dev edition-universal edition-server

# --- EchOS editions ----------------------------------------------------------

.PHONY: stage-epk-media
# Staged into the ISO but not compiled: without these as prerequisites an edit
# to a manifest or the package index ships inside a stale image.
ISO_STAGE_FILES := media/epk/INDEX editions/minimal.manifest \
                   editions/complete.manifest editions/edge.manifest \
                   editions/server.manifest

stage-epk-media:
	@mkdir -p $(ISODIR)/media/epk
	cp editions/minimal.manifest editions/complete.manifest \
	   editions/edge.manifest editions/server.manifest \
	   $(ISODIR)/media/epk/
	cp media/epk/INDEX $(ISODIR)/media/epk/INDEX 2>/dev/null || true

$(ISO_VM): $(KERNEL_ELF) $(KERNEL_BIN) $(ISO_STAGE_FILES) $(GRUB_VM_SRC)
	@test -n "$(GRUB_MKRESCUE)" || { echo "grub-mkrescue not found (dnf install grub2-tools-extra xorriso)"; exit 1; }
	@rm -rf $(ISODIR)
	@mkdir -p $(ISODIR)/boot/grub
	cp $(KERNEL_ELF) $(ISODIR)/boot/rxos.elf
	cp $(KERNEL_BIN) $(ISODIR)/boot/rxos.bin
	$(MAKE) --no-print-directory ISODIR="$(ISODIR)" stage-epk-media
	sed -e 's/@VER@/$(ECHOS_VER)/g' -e 's/@EDITION@/$(ECHOS_EDITION_UP)/g' $(GRUB_VM_SRC) > $(ISODIR)/boot/grub/grub.cfg
	cp boot/grub/eclipse.jpg $(ISODIR)/boot/grub/eclipse.jpg
	$(GRUB_MKRESCUE) -o "$@" $(ISODIR) --modification-date=$(ISO_DATE) --set_all_file_dates $(ISO_DATE) -r
	@python3 tools/iso_normalize.py "$@" $(SOURCE_DATE_EPOCH)
	@echo "[rxos] built $@  (QEMU / VirtualBox)"

$(ISO_METAL): $(KERNEL_ELF) $(KERNEL_BIN) $(ISO_STAGE_FILES) $(GRUB_METAL_SRC)
	@test -n "$(GRUB_MKRESCUE)" || { echo "grub-mkrescue not found"; exit 1; }
	@rm -rf $(ISODIR)
	@mkdir -p $(ISODIR)/boot/grub
	cp $(KERNEL_ELF) $(ISODIR)/boot/rxos.elf
	cp $(KERNEL_BIN) $(ISODIR)/boot/rxos.bin
	$(MAKE) --no-print-directory ISODIR="$(ISODIR)" stage-epk-media
	sed -e 's/@VER@/$(ECHOS_VER)/g' -e 's/@EDITION@/$(ECHOS_EDITION_UP)/g' $(GRUB_METAL_SRC) > $(ISODIR)/boot/grub/grub.cfg
	cp boot/grub/eclipse.jpg $(ISODIR)/boot/grub/eclipse.jpg
	$(GRUB_MKRESCUE) -o "$@" $(ISODIR) --modification-date=$(ISO_DATE) --set_all_file_dates $(ISO_DATE) -r
	@python3 tools/iso_normalize.py "$@" $(SOURCE_DATE_EPOCH)
	@echo "[rxos] built $@  (real PC / USB: dd if=$@ of=/dev/sdX bs=4M status=progress)"

# --- run in QEMU (amnesic: read-only ISO, nothing persisted) ------------------
$(ISO_VM): ISODIR := $(BUILD)/isodir-vm
$(ISO_METAL): ISODIR := $(BUILD)/isodir-metal

# The desktop IS the shell (one UI with a command bar), so run/run-shell/
# run-desktop boot the same image; the aliases exist for discoverability.
#
# NIC is always attached: SLIRP user-net (host NAT → guest internet) + virtio-net.
# Guest defaults for WWW: 10.0.2.15/24 gw 10.0.2.2 dns 10.0.2.3  (`www on`).
# Override: make run QEMU_NET=   (empty = no NIC) or set QEMU_NET yourself.
QEMU_NET_VIRTIO := -netdev user,id=net0 -device virtio-net-pci,netdev=net0,bus=pcie.0,mac=52:54:00:12:34:56
QEMU_NET_E1000  := -netdev user,id=net0 -device e1000,netdev=net0,bus=pcie.0,mac=52:54:00:12:34:56
QEMU_NET ?= $(QEMU_NET_VIRTIO)
# PS/2 mouse (relative + wheel). Do not add usb-tablet / virtio-tablet: those
# are absolute pointers with no Z axis, so the guest never sees the wheel.
QEMU_FLAGS := $(QEMU_HOST) -machine q35 -m 512M -serial stdio -no-reboot -no-shutdown $(QEMU_NET)
run: $(ISO)
	@echo "[rxos] QEMU + virtio-net (user/NAT) — inside OS: www on"
	$(QEMU) $(QEMU_FLAGS) -cdrom "$(ISO)"

# Same ISO, Intel e1000 (the metal/laptop path). Kernel probes virtio, e1000, r8169, rtl8139.
run-e1000: $(ISO)
	@echo "[rxos] QEMU + e1000 (user/NAT) — inside OS: www on"
	$(QEMU) $(QEMU_HOST) -machine q35 -m 512M -serial stdio -no-reboot -no-shutdown \
	    $(QEMU_NET_E1000) -cdrom "$(ISO)"

# MacBook Air M1 / Apple Silicon: same ISO, TCG, cocoa window.
run-macos: $(ISO)
	@chmod +x tools/run_qemu_macos.sh
	tools/run_qemu_macos.sh "$(ISO)"

run-shell: run
run-desktop: run

run-fs: $(ISO)
	@echo "[rxos] QEMU + virtio-net (user/NAT) — inside OS: www on"
	$(QEMU) $(QEMU_FLAGS) -cdrom "$(ISO)" -full-screen

# Boot straight from the ELF via QEMU's Multiboot loader (skips ISO authoring).
run-elf: $(KERNEL_ELF)
	@echo "[rxos] QEMU + virtio-net (user/NAT) — inside OS: www on"
	$(QEMU) $(QEMU_FLAGS) -kernel $(KERNEL_ELF)

# --- disk image (RXFS persistence; attach alongside the live ISO) ------------
disk:
	tools/make_disk_image.sh $(DISK_IMG)

run-disk: $(ISO)
	tools/run_qemu_disk.sh "$(ISO)" $(DISK_IMG)

# --- bootable hard disk (isohybrid; use without USB/CD) ---------------------
# Produces build/rxos-hdd.img from the hybrid ISO. Flash to a real drive:
#   sudo dd if=build/rxos-hdd.img of=/dev/sdX bs=4M status=progress conv=fsync
# Or test: make run-hdd
hdd: $(ISO)
	@chmod +x tools/make_bootable_disk.sh
	tools/make_bootable_disk.sh $(HDD_IMG) 256 $(ISO)

run-hdd: hdd
	@chmod +x tools/run_qemu_hdd.sh
	tools/run_qemu_hdd.sh $(HDD_IMG)

capture: $(KERNEL_ELF)
	@echo "[rxos] capture: F12 or shell command 'capture' -> /screenshots/sNN.bmp"

# --- automated QEMU smoke test ------------------------------------------------
# The same ISO, certified under both firmwares. OVMF is a host package; the
# image is one hybrid file either way.
OVMF_CODE ?= /usr/share/edk2/ovmf/OVMF_CODE.fd
OVMF_VARS ?= /usr/share/edk2/ovmf/OVMF_VARS.fd
test: $(ISO)
	python3 tools/smoke_universal.py --iso "$(ISO)" --edition "$(EDITION)" --version "$(ECHOS_VER)" --firmware bios --output "$(BUILD)/cert21" --qemu "$(QEMU)"

# Portability gate: no generic module may reach for this machine. Runs on
# its own for a quick answer; the certificate runs it too.
.PHONY: portability
portability:
	python3 tools/portability_gate.py

# CERT-G: the AArch64 board, asked of itself. The image (not the ELF) is the
# subject: only it carries the boot header that gets a device tree passed in.
.PHONY: build-universal-aarch64 test-universal-aarch64 test-px4-universal-aarch64 \
        test-uefi-universal-aarch64 esp-universal-aarch64 run-universal-aarch64
build-universal-aarch64:
	$(MAKE) ARCH=aarch64 EDITION=universal $(BUILD_ROOT)/aarch64/universal/rxos.img

# The same image, on an EFI system partition. Nothing is converted: the file
# copied to BOOTAA64.EFI is byte for byte the one QEMU boots with -kernel,
# because it carries both headers.
AAVMF_CODE ?= /usr/share/edk2/aarch64/QEMU_EFI-silent-pflash.raw
AAVMF_VARS ?= /usr/share/edk2/aarch64/vars-template-pflash.raw
ESP_IMG    := $(BUILD_ROOT)/aarch64/universal/esp.img
esp-universal-aarch64: build-universal-aarch64
	@test -f $(AAVMF_CODE) || { echo "no AArch64 UEFI firmware at $(AAVMF_CODE)"; exit 1; }
	rm -f $(ESP_IMG)
	dd if=/dev/zero of=$(ESP_IMG) bs=1M count=48 status=none
	mformat -i $(ESP_IMG) -F -v ECHOS ::
	mmd -i $(ESP_IMG) ::/EFI ::/EFI/BOOT
	mcopy -o -i $(ESP_IMG) $(BUILD_ROOT)/aarch64/universal/rxos.img ::/EFI/BOOT/BOOTAA64.EFI
	@echo "[rxos] ESP $(ESP_IMG)"
test-universal-aarch64: build-universal-aarch64
	python3 tools/smoke_aarch64.py --image $(BUILD_ROOT)/aarch64/universal/rxos.img \
		--output $(BUILD_ROOT)/aarch64/cert
test-uefi-universal-aarch64: esp-universal-aarch64
	python3 tools/smoke_aarch64.py --image $(BUILD_ROOT)/aarch64/universal/rxos.img \
		--output $(BUILD_ROOT)/aarch64/cert-uefi --firmware uefi \
		--esp $(ESP_IMG) --aavmf-code $(AAVMF_CODE) --aavmf-vars $(AAVMF_VARS)
test-px4-universal-aarch64: build-universal-aarch64
	python3 tools/smoke_aarch64.py --image $(BUILD_ROOT)/aarch64/universal/rxos.img \
		--output $(BUILD_ROOT)/aarch64/cert --px4
run-universal-aarch64: build-universal-aarch64
	qemu-system-aarch64 -machine virt -cpu cortex-a72 -m 512M -nographic \
		-kernel $(BUILD_ROOT)/aarch64/universal/rxos.img \
		-netdev user,id=n0 -device virtio-net-device,netdev=n0

# PX4 certification: the flight stack on the host, echOS in the VM, one wire.
# PX4 SITL is a host tool and is never part of the ISO.
PX4_ROOT ?= /home/roger/px4/PX4-Autopilot
.PHONY: test-px4 test-px4-universal-x86_64
test-px4: $(ISO)
	@test -x $(PX4_ROOT)/build/px4_sitl_default/bin/px4 || \
	  { echo "PX4 SITL not built at $(PX4_ROOT) (see docs/CERT_E_PX4.md)"; exit 1; }
	python3 tools/px4_test.py --iso "$(ISO)" --output "$(BUILD)/certE" --qemu "$(QEMU)"
test-px4-universal-x86_64:
	$(MAKE) ARCH=x86_64 EDITION=universal test-px4

# Storage certification: a real NVMe controller, a real GPT, a real reboot.
.PHONY: test-storage test-storage-universal-x86_64 test-uefi run-uefi
test-storage: $(ISO)
	python3 tools/storage_test.py --iso "$(ISO)" --output "$(BUILD)/cert22" --qemu "$(QEMU)"
test-storage-universal-x86_64:
	$(MAKE) ARCH=x86_64 EDITION=universal test-storage
test-uefi: $(ISO)
	python3 tools/smoke_universal.py --iso "$(ISO)" --edition "$(EDITION)" --version "$(ECHOS_VER)" --firmware uefi --output "$(BUILD)/cert21-uefi" --qemu "$(QEMU)"

# Boot the same ISO through OVMF by hand. Variables are copied per run so a
# stale boot entry cannot decide what starts.
run-uefi: $(ISO)
	@test -f $(OVMF_CODE) || { echo "OVMF not found at $(OVMF_CODE) (dnf install edk2-ovmf)"; exit 1; }
	@cp $(OVMF_VARS) $(BUILD)/OVMF_VARS.fd
	$(QEMU) $(QEMU_FLAGS) \
	    -drive if=pflash,format=raw,unit=0,readonly=on,file=$(OVMF_CODE) \
	    -drive if=pflash,format=raw,unit=1,file=$(BUILD)/OVMF_VARS.fd \
	    -cdrom "$(ISO)"

# --- automated v4.1 persistence test (double boot on one disk image) ----------
test-disk: $(ISO)
	tools/persist_test.sh "$(ISO)"

# --- QOI decoder test (host-side; compiles the kernel source verbatim) --------
qoi-test: | dirs
	gcc -O2 -Wall -Wextra -o $(BUILD)/qoi_host_test tools/qoi/qoi_host_test.c
	@if [ -f assets/wallpaper/pack/paper29.jpg ]; then \
	  gcc -std=c17 -O2 -DJPEG_HOST -o $(BUILD)/jpeg_host_test tools/jpeg_host_test.c; \
	  $(BUILD)/jpeg_host_test assets/wallpaper/pack/paper29.jpg; \
	fi

# --- Rogex Wired protocol test (host-side, deterministic; no QEMU) -------------
net-test-host: | dirs
	gcc -std=c17 -Idrivers/net -Iuserland/runtime -Ikernel \
	    -o $(BUILD)/wired_hosttest tools/wired_hosttest.c userland/runtime/wired.c
	$(BUILD)/wired_hosttest

# --- two-node Rogex Wired ping (needs QEMU networking; not sandbox-friendly) ---
net-test: $(ISO)
	tools/net_test.sh "$(ISO)"

# --- same two-node ping over the e1000 (VirtualBox's default 82540EM NIC) ------
net-test-e1000: $(ISO)
	NIC=e1000 tools/net_test.sh "$(ISO)"

# --- regenerate embedded app header from userland/apps/*.rxc -------------------
apps:
	python3 tools/mkapps.py

# --- toolchain check -----------------------------------------------------------
check:
	tools/check_toolchain.sh

# --- REVM fuzzing (host: libFuzzer + ASan/UBSan) -------------------------------
FUZZ_BIN  := $(BUILD)/fuzz_revm
FUZZ_TIME ?= 60
$(FUZZ_BIN): userland/runtime/revm.c tests/fuzz_revm.c | dirs
	clang -g -O1 -fsanitize=address,undefined,fuzzer -Ikernel -Iuserland/runtime \
	      userland/runtime/revm.c tests/fuzz_revm.c -o $@

fuzz: $(FUZZ_BIN)
	@mkdir -p $(BUILD)/fuzz_corpus
	$(FUZZ_BIN) -max_total_time=$(FUZZ_TIME) -print_final_stats=1 $(BUILD)/fuzz_corpus

# --- content packages: pack site dirs into .rgxsite + embed as C headers -------

clean:
	rm -rf $(BUILD)
	cd rogex-core && $(CARGO) clean
