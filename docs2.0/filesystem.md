# Sistema de ficheros

RXFS. Nativo. No ext4, no FAT (salvo lo que el instalador ponga en
el MBR alrededor). VFS encima: `vfs_read`, `vfs_write`, paths.

## Árbol 2.0

```text
/
├── boot/                 kernel, GRUB
├── sys/
│   ├── editions/
│   ├── modules/
│   └── network/
├── bin/
├── etc/                  doas.conf pf.conf sudoers echos-edition os-release
├── epk/
│   ├── db/
│   └── manifests/
├── var/
│   ├── log/
│   └── run/
├── dev/
├── users/                ← no /home
│   └── <nombre>/
│       ├── Desktop
│       ├── Documents
│       ├── Downloads
│       └── …
└── tmp/
```

Lo siembra `userland/cli/layout.c`. Idempotente. `HOME=/users/<user>`.
`OS=echOS 2.1`.

No hay `/usr`. No hay `/usr/local`. No hay `/home`. Scripts de 1.0
que cachen `/home` están mal en 2.x.

## Tamaño (esta generación)

64 ficheros. 64 KiB por fichero. En serio.

Por eso `curl -o` de una portada HTML grande se trunca al escribir.
Por eso CPython no vive en RXFS. Cuando la ISO llevaba pesos de medio
mega, iban por módulo GRUB por esta misma razón; ya no los lleva.

Cuando suba esos números, este párrafo se reescribe. Hasta entonces
es el techo.

## Persistencia

Por defecto: **amnésico**. LIVE es RAM.

Con disco instalado: `save` escribe la imagen RXFS, `load` la lee.
`format <dev> yes` la vacía. El boot puede restaurar si hay imagen
válida (“Welcome back” en 1.0; en 2.0 el LIVE no lo espera).

MBR + una partición EchOS. `partition`, `cfdisk`, `gpt`, `disklabel`
existen y **borran**. No son un juego.

## Permisos

Unikernel: un espacio. `/etc/sudoers` y `doas.conf` son texto de
política. `/tmp` está anotado 1777 en el mapa de `layout.c`; no hay
DAC de Linux.

Usuario `live` en LIVE: NOPASSWD en esa lista. No significa que
haya dos anillos.

## Nombres de directorio

El wizard en español te deja Escritorio/Documentos/…; en inglés
Desktop/Documents. Mismo sitio, etiquetas. `lang` del wizard.

## `/dev`

Nodos sintéticos: thermal, rapl, etc. `dev` / `cat /dev/…`. No udev.

## Copias de seguridad

No hay `rsync`. `save` es el snapshot. Fuera: `dd` del disco. RXFS
no es un FS que montes en Linux (hoy). El host ve un blob.

— R.N.
