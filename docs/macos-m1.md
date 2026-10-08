# rxOS 6.5 en macOS — MacBook Air M1 (Apple Silicon)

RXos es un guest **x86_64**. En un Air M1/M2/M3, QEMU **no puede** usar HVF
para eso (HVF solo acelera invitados ARM). El camino correcto es **TCG**:
emulación de software. Es más lento que Fedora+KVM; el escritorio arranca
igual.

Máquina de referencia: **MacBook Air (M1)**.

## 1. Instalar QEMU

```bash
# Homebrew: https://brew.sh
/bin/bash -c "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)"
echo 'eval "$(/opt/homebrew/bin/brew shellenv)"' >> ~/.zprofile
eval "$(/opt/homebrew/bin/brew shellenv)"

tools/setup-macos.sh          # instala qemu
# opcional, para compilar el kernel en el Mac:
tools/setup-macos.sh --build
```

## 2. Conseguir la ISO VM

```
build/rxOS-6.5.0-vm.iso
```

Esa es la ISO de **emulador**, no la metal. En el Air:

- Si ya está en el repo (la construiste en Linux / CI): úsala.
- Si no: en un Linux `make iso-vm` y copia el archivo al Mac.
- `make iso` en Homebrew ARM a menudo **falla**: no hay módulos GRUB i386-pc.
  Entonces `make run-elf` (Multiboot, sin ISO) si el toolchain `--build` está.

## 3. Arrancar

```bash
make run-macos
# o:
tools/run_qemu_macos.sh build/rxOS-6.5.0-vm.iso
```

- Ventana **cocoa** = pantalla (clic para el ratón).
- Esta terminal = consola serie.
- Primera vez en TCG: espera ~20–40 s al menú GRUB / escritorio.

Dentro del OS:

```text
www on
nics
rx app add fortune
fortune
```

RAM por defecto en el script macOS: **1G** (`RXOS_QEMU_MEM=2G` si quieres).

## 4. Qué no hacer

| Evitar | Por qué |
|---|---|
| `qemu-system-aarch64` | RXos no es ARM |
| `-accel hvf` con esta ISO | HVF no emula Intel en M1 |
| ISO **metal** en QEMU | Lista VESA de portátil; usa la **vm** |
| UTM “ARM Linux” | Otro ISA. Si usas UTM, elige emulación **x86_64**, no Virtualize |

## 5. Compilar en el Mac (opcional)

```bash
tools/setup-macos.sh --build
source ~/.rogex-env
make                 # → build/rxos.elf
make run-elf         # QEMU Multiboot, sin grub-mkrescue
```

`make iso-vm` solo si `grub-mkrescue` existe y trae `i386-pc`. Si no, construye
la ISO en Fedora y tráela.

## 6. Persistencia en el Mac

```bash
make disk
make run-disk
```

`tools/run_qemu_disk.sh` ya añade TCG en Darwin.

## Relación con otras ISOs

| Archivo | Dónde |
|---|---|
| `build/rxOS-6.5.0-vm.iso` | **Este Mac** (QEMU TCG) y VirtualBox x86 |
| `build/rxOS-6.5.0-metal.iso` | PC real / USB, no el Air |

Más: [ISOS.md](ISOS.md), [INSTALL.md](INSTALL.md).
