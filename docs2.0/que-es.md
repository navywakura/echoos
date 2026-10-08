# Qué es echOS 2.1

**Rogex Laboratories · septiembre 2026**

echOS 2.1.0-honest es un sistema operativo **bare-metal x86_64**. Unikernel
de consola. Un ELF. GRUB Multiboot2. No hay Linux debajo. No hay busybox.
No hay systemd. La máquina *es* el programa.

No es un OS comercial. No es un navegador. Está pensado para **software
neuromórfico y robótica**, y para alojar más adelante Echo AI (hoy ese
código es host y no va en esta ISO). Akida AKD1000 es una sonda PCI para
cuando haya placa.

La línea 1.0 (ECLIPSE) es un escritorio: dock, ventanas, Nova, Eclipse Shell.
2.x es **otra línea**. Misma familia, mismo laboratorio, otro producto.

Slogan en `kernel/version.h`:

> Console unikernel. Neuromorphic software. Akida when the card exists.

## Por qué existe

El escritorio de 1.0 me sirve para enseñar el laboratorio y para el portátil
con ratón. No me sirve para:

- un LIVE que cabe en RAM y no escribe disco,
- un Edge con Akida y sin WM,
- un Server que no arrastra un compositor,
- un instalador TUI que no depende de “aceptar la licencia en una ventana”.

Quería **un kernel Heap-0 compartido** y que la edición se elija *al
instalar*, no al compilar cuatro árboles. Eso es 2.0.

## Qué es, en una frase

Una consola Liberation Mono sobre framebuffer (o VGA texto), con ROSH, `epk`
local, pila IPv4/DNS/TCP/HTTP(S), herramientas estilo OpenBSD, y el mismo
tejido de eventos / SNN que ya venía de rxOS — sin ventana, sin dock, sin
mentir sobre el hardware.

## Qué no es

- No es “EchOS 1.0 con `startx` desactivado”.
- No es Alpine, no es OpenBSD, no es Haiku. Me inspiré en OpenBSD para
  *nombres* (`doas`, `rcctl`, `pfctl`). El kernel es mío.
- No es un desktop. Si quieres el dock, usa 1.0 Complete.
- No es un Linux embebido. Si buscas `apt`, cierra esto.

## Piezas que importan

| Pieza | Qué es |
|---|---|
| Heap-0 | Layout estático en BSS. O(1). Cero fragmentación en el camino caliente. |
| RXFS | FS nativo, slots fijos. No es ext4. 64 ficheros × 64 KiB en esta generación. |
| `epk` | Notas en RXFS. `epk list` = lo que el kernel tiene. `--lab` = host. `epk get` es HTTP opcional (`www on`). |
| LIVE | Arranca en RAM. Minimal. `q` en el wizard y estás dentro. |
| Ediciones | Minimal / Complete / Edge / Server — un manifiesto, no un fork. |
| CLI | Liberation Mono, Braille del logo, `termtheme`, `man`, `nano`. |
| Red | `www on`, `curl`, `wdl`, `ipconf`. HTTPS ofrece TLS 1.3 y cae a 1.2. |

## Cómo se llama de verdad

En el código viejo verás `rxOS`, `RXos`, `EchOS`. El producto de esta ISO
se llama **echOS 2.1.0-honest**. El banner de boot dice `[echOS]`. `uname`
también. `/etc/os-release` lo mismo.

El repositorio sigue llamándose RXos porque así nació. No voy a reescribir
la historia del git para quedar bonito.

— R.N.
