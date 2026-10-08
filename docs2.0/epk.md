# epk — notas de paquete

`epk` es el gestor de 2.1. No es `apt`. No es `pkg_add` de OpenBSD,
aunque los subcomandos se parezcan. Por defecto **no abre sockets**.
`epk get` sí hace HTTP si `www on` — es opcional y cabe en 64 KiB.

## Por qué local

En 1.0 el canal `https://www.rogexlaboratories.com/rx-os/packages`
me parecía cómodo y me acabó oliendo a “el OS llama a casa”. En 2.0
el catálogo viaja **en la ISO** (`/media/epk`, `media/epk/INDEX`).
El kernel tiene un mirror en `userland/cli/epk.c`. Si no está en el
medio, no se instala. Punto.

## Comandos

```text
epk list               # notas del kernel (lo que este unikernel tiene)
epk list --lab         # toolchain / daemons de host (NOTA, no corren aquí)
epk info [pkg]
epk search <palabra>
epk install <pkg>      # alias: epk_add  (escribe texto en RXFS)
epk remove <pkg>       # alias: epk_delete
epk apply <edition>    # minimal|complete|edge|server (solo notas públicas)
epk get <pkg>          # HTTP opcional, `www on`
epk stress [N]         # install/remove, delta Heap-0
```

`epk apply` lee `editions/<ed>.manifest` y mete todo lo que quepa.

## Qué es un “paquete” aquí

RXFS no traga CPython. Un “paquete” 2.1 es **nota**: nombre + texto en
`/bin` + fila en `/epk/db`. El ELF de verdad, si existe, se enlaza en
**host** contra `echlibc` (`packages/`).

`epk list` ya no enseña `tcc` ni `httpd`. Esos nombres viven en
`epk list --lab`. Si los escribes en el prompt LIVE, no hay stub de
compilador: el comando no está. Eso es 2.1.

## Dónde vive

```text
/epk/db            registros
/epk/manifests     edición activa
/bin               notas / stubs
/media/epk         catálogo ISO (GRUB module / isodir)
```

Formato host: RXP1, el mismo que `rx-pkg` de 1.0 (`packages/SPEC.md`).
Binario de Linux: `packages/bin/epk`.

## LIVE

`epk_live_all()` marca las notas **públicas** como disponibles.
No instala el catálogo `--lab`. No significa que CPython quepa.

## Relación con `rx app`

`rx app add` es el canal legado, *opcional*, y pide `www on`. En 2.0
no es el camino. Lo dejo por no romper scripts de 1.0. El camino es
`epk`.

## echlibc

`packages/echlibc/`. libc mínima para ELF estáticos *de host hacia
echOS*, no para el kernel. Syscalls de mentira: `sys_open` `sys_read`
`sys_write` `sys_mmap` `sys_spawn`. `mmap` = ventana BSS 64 KiB.
No hay `malloc` de glibc en ese contrato. Los `packages/coretools/*`
en Linux *sí* usan malloc: son binarios de Fedora, fuera de Heap-0.

Triples que quiero (aún host `x86_64-unknown-none`):

- `x86_64-unknown-echos`
- `aarch64-unknown-echos`
- `i686-unknown-echos`

Ver `docs/ECHOS_TARGET.md`.

— R.N.
