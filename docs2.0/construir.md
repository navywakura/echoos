# Construir echOS 2.1

Host típico: Linux x86_64, clang, nasm, ld.lld, grub2-mkrescue,
xorriso, qemu-system-x86_64, rustc/cargo para `rogex-core`.

```sh
make EDITION=universal
make EDITION=universal iso-vm iso-metal
make EDITION=universal run
```

Salida:

```text
build/rxos.elf
build/rxos.bin
build/EchOS-2.0.0-universal-vm.iso
build/EchOS-2.0.0-universal-metal.iso
```

## Flags que importan

`EDITION=universal` define `-DECHOS_EDITION_UNIVERSAL=1` →
`ECHOS_CLI_ONLY`, `ECHOS_VERSION 2.0.0`, sin desktop.

Otras: `complete` `minimal` `edge` `server` `dev`. Dev activa
atajos de teclado que en usuario real no quiero.

## Herramientas

```text
clang --target=x86_64-none-elf
nasm -f elf64
ld.lld -T linker.ld
llvm-objcopy -O binary
grub-mkrescue | grub2-mkrescue
```

`CFLAGS`: freestanding, no pic, no red zone, no sse, mcmodel
kernel, C17, `-O2`. Headers: cada directorio de `C_DIRS` en el
Makefile.

Rust: `rogex-core` target `x86_64-unknown-none` `--release`.
Se enlaza el `.a`.

## Dónde está el 2.0 en el árbol

```text
userland/cli/          wizard tui epk echofetch nano man netcli live
drivers/npu/akida.*
arch/echos_arch.h
editions/*.manifest
boot/grub/grub-universal.cfg
packages/echlibc/
packages/epk/
media/epk/
docs2.0/               esto
```

`docs/` (sin 2.0) es el archivo histórico. No lo uses como
especificación de Universal.

## Tests que yo corro

```sh
make test-universal-x86_64     # CERT-21: arranca la ISO en QEMU y afirma
make repro-universal-x86_64    # dos builds limpios -> mismos sha256
```

`make test` arranca la imagen de verdad, teclea comandos y espera la
respuesta de cada uno: identidad compilada, GDT/IDT, memoria, timer, SEB,
RXFS, shell, ficheros con bytes, límites declarados, una petición HTTP
real contra un listener del host, y que ni la ISO ni la sesión nombren lo
que se retiró. El informe queda en `$(BUILD)/cert21/report.json`.

El smoke viejo de 1.x (`/home`, halt de RXos 7) está en
`legacy/tools-navi/smoke_test.sh`. No es la biblia de esta línea.

## Host helpers

```sh
cc -std=c99 -o echos-install tools/echos-install-host.c
cd packages && make && make test    # coretools Linux, 1.0/2.0
```

## macOS

Apple Silicon: TCG, `make run-macos`. No HVF. Ver `docs/macos-m1.md`.

## No hagas

- `make` sin `EDITION=universal` y esperar 2.0 (cae a Minimal 1.0).
- Flashear la ISO *vm* a un USB y extrañarse del VESA.
- Montar NVMe y abrir issue de “no instala” — no hay driver.
- Medir Heap-0 con un `kmalloc` en el medio y citar el slogan.

## Licencia

GNU GPLv3. El código es el árbol. Ya no hay blobs de pesos: el
chatbot que los cargaba por módulo GRUB salió del producto y la ISO
sólo lleva el kernel, su payload y el índice `epk`.

Sitio: https://www.rogexlaboratories.com

— R.N.
