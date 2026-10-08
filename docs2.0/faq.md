# Preguntas que me hacen

## ¿Esto es Linux?

No. Unikernel x86_64. GRUB carga un ELF. No hay kernel.org.

## ¿Dónde está el escritorio?

En EchOS 1.0 Complete/Minimal. 2.0 Universal es consola. A propósito.

## ¿Por qué no `/home`?

Porque no soy Unix. Los usuarios viven en `/users`. `HOME` apunta ahí.

## ¿Puedo instalar paquetes de internet?

`epk` no. El catálogo es la ISO. `curl`/`wdl` bajan *ficheros*, no
paquetes firmados. `rx app add` es legado 1.0 y pide `www on`; no es
el camino 2.0.

## `tcc` no está en `epk list`

A propósito. `epk list` son notas del kernel. Toolchain de host:
`epk list --lab`. RXFS es 64 KiB/archivo. El compilador, si existe,
se enlaza en host contra `echlibc`. Si escribes `tcc` en LIVE:
comando ausente, no un usage de compilador.

## ¿Hay WiFi?

No. Ethernet. `nics` te enseña el chip WiFi para que sepas que lo
vi y no lo conduzco.

## `ping google.com` falla en QEMU

slirp filtra ICMP. Prueba `dns google.com` o `curl -I http://example.com`.

## `curl https://…` no imprime HTML

HTTP `http://` sí. TLS 1.3 *handshake* sí (`tls example.com`). El
cuerpo https todavía puede salir vacío. No es tu URL. Ver [red.md](red.md).

## `-dom` no “carga Vite”

No hay motor JS. Espera dos segundos y re-pide el documento. El
cascarón de una SPA es lo que hay.

## ¿Cómo completo comandos?

Tab. La lista es `CMD_NAMES`. Si no completa, el comando no está
en el array (bug) o escribiste mal.

## ¿`man` es el man de Linux?

No. Páginas embebidas. Sin secciones reales. `man 1 curl` ignora
el `1`.

## ¿Puedo perder el disco?

Sí. `install`, `reinstall`, `format … yes`, `ERASE-`. LIVE con `q`
no toca disco. No mezcles.

## ¿Akida está soportado?

Sonda PCI. Sin placa no hay NPU. No pinto uno falso.

## ¿2.0 sustituye a 1.0?

No. Son líneas. 1.0 es el Eclipse Shell. 2.0 es el LIVE / Edge /
Server de consola. Mismo laboratorio, distinto ISO.

## SHA-256 de la ISO

Las ISOs no van en git. Tras `make edition-universal && make iso`:

```sh
sha256sum build/EchOS-2.1.0-universal-*.iso
```

No hay descarga pública 2.x hasta Akida medido. Si alguien te pasa una
ISO sin suma, no es un release mío en serio.

## ¿Puedo usarlo para navegar / venderlo como OS de escritorio?

No. Investigación, robótica, SNN. `curl` no es un navegador. 1.0 es
el escritorio, y es otra ISO.

— R.N.
