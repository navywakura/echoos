# Instalar echOS 2.1

Hay tres maneras de estar dentro, y no son equivalentes.

1. **LIVE** — RAM. Amnésico. Lo que quiero que pruebes primero.
2. **Instalado en disco** — MBR + kernel + RXFS. Destructivo.
3. **Recovery / memtest** — GRUB, entradas 2 y 3.

## Las ISOs

```text
build/EchOS-2.0.0-universal-vm.iso      QEMU / VirtualBox
build/EchOS-2.0.0-universal-metal.iso   PC real / USB
```

En 2.0 el cfg de GRUB es el mismo (`grub-universal.cfg`). Aun así:
usa *vm* en el emulador y *metal* en el portátil. VESA y timeouts
están pensados para cada sitio.

Generarlas:

```sh
make EDITION=universal iso-vm iso-metal
```

~36 MiB cada una. El ELF ~4.5 MiB. No van en git.

## QEMU (el camino de diario)

```sh
qemu-system-x86_64 -machine q35 -m 512M -accel kvm \
  -cdrom build/EchOS-2.0.0-universal-vm.iso \
  -serial stdio \
  -netdev user,id=net0 \
  -device virtio-net-pci,netdev=net0,bus=pcie.0,mac=52:54:00:12:34:56
```

O `make EDITION=universal run` si no quieres pelearte con flags.

En el TUI: **q** = LIVE Minimal. Enter recorre idioma → zona → teclado
→ locale → edición → disco.

Serial es una consola de verdad. Los tests del repo alimentan COM1.

### Red en QEMU

slirp: invitado `10.0.2.15/24`, gw `10.0.2.2`, DNS `10.0.2.3`.
`www on` lo pone si DHCP falla (OUI 52:54:00 o driver virtio/e1000).

ICMP a menudo lo filtra slirp. `ping google.com` puede caer a UDP/53.
No es que tu cable esté mal.

### Disco en QEMU (opcional)

Sin virtio-blk el LIVE es más simple (menos IRQ). Si montas bloque:

```sh
qemu-img create -f raw build/rxos-disk.img 64M
# -device virtio-blk-pci,drive=...
```

El driver enmascara INTx y trabaja polled. Si el boot se queda en
`nic` para siempre, es IRQ compartida: esa máscara está precisamente
por eso.

## USB / metal

```sh
lsblk
sudo dd if=build/EchOS-2.0.0-universal-metal.iso of=/dev/sdX bs=4M status=progress conv=fsync
```

BIOS/CSM Legacy. UEFI nativo no está. En el HP 15-ac195nl: Ethernet
cable, no WiFi. `nics` lista `10EC:8136` (r8169) y el WiFi `10EC:8179`
que no conduzco.

## El wizard

Orden:

1. Idioma (English / Español)
2. Zona horaria
3. Teclado en/es
4. Locale
5. Edición: Minimal, Complete, Edge, Server, o LIVE
6. Disco (si no es LIVE) — frase `ERASE-` o `REINSTALL-`

Desde un LIVE ya dentro: `echos-install` vuelve a abrirlo.

Host (sin QEMU, preview del TUI):

```sh
cc -std=c99 -Wall -o echos-install tools/echos-install-host.c
./echos-install
```

No formatea tu Fedora. Es el menú, nada más.

## LIVE vs instalado

| | LIVE | Disco |
|---|---|---|
| Persistencia | no (salvo `save` a un disco que ya exista) | `save` / `load` |
| Usuario | `live` | el del wizard |
| Paquetes | catálogo entero *visible*; toolchain a menudo stub | `epk apply <edición>` |
| Peligro | bajo | alto (MBR) |

`format <dev> yes` vacía RXFS. `reinstall <dev>` pisa GPT/MBR viejo.
No los ejecutes “para ver qué pasa”.

## GRUB

Timeout 2 s. Entradas:

1. Installer / LIVE
2. Memory Test
3. Recovery
4. text console only (mismo live, `gfxpayload=text`)

Recovery: sin wizard. `load` / `save`, `install`, `epk`, `disklabel`.

## macOS Apple Silicon

x86_64 no entra en HVF. TCG:

```sh
make EDITION=universal run-macos
```

Más lento. Paciencia en el TUI.

## Si no arranca

- QEMU: máquina `q35`, no `pc` a ciegas (PCI de virtio).
- Metal: CSM on, Secure Boot off.
- Pantalla negra con serial vivo: framebuffer VESA no negoció; la
  entrada 4 (text) te saca.
- Cuelgue tras NIC: ISO nueva (máscara INTx del blk). Si es ISO de
  hace dos días, recompila.

— R.N.
