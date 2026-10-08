# Novedades respecto a EchOS 1.0

**Qué cambió de ECLIPSE (1.0) a Universal (2.x / 2.1.0-honest).**

1.0 no está muerto. Complete / Minimal / Edge de 1.x siguen siendo
escritorio (o CLI Edge) con flags de compilación. 2.0 es la línea que
yo uso ahora para LIVE, metal y laboratorio.

## De un vistazo

| | EchOS 1.0 ECLIPSE | echOS 2.1 Universal |
|---|---|---|
| Interfaz | Escritorio Eclipse, dock, ventanas | Consola. Cero WM. |
| Tipografía | Mixto; el FB teñía todo de verde cyberpunk | Liberation Mono. Paleta del CLI (`termtheme`). |
| Kernel | Un ELF **por** edición (flags `-D`) | **Un** ELF Heap-0. Edición = manifiesto `epk`. |
| Usuarios | `/home` | `/users` |
| Jerarquía | restos de Unix (`/usr` a veces) | `/boot /sys /bin /etc /epk /var /dev /users /tmp` |
| Instalador | wizard gráfico | TUI 80×25, ES/EN, `q` = LIVE |
| GRUB | menú VM vs metal | LIVE / memtest / recovery en el mismo cfg |
| Paquetes | `rx-pkg` / canal HTTPS opcional | `epk` **solo local** (`/media/epk`, `/epk`) |
| Manual | `help` truncado | `man` / `manual` / `apropos` por comando |
| HTTP | `curl` básico, `wget`, TLS 1.2 | `curl` con flags, `wdl`, TLS 1.3 ofrecido |
| Red vista | `nics` (PCI) | `ipconf`: driver, MAC, IPv4, gw, DNS |
| Editor | `write` / `scribe` | `nano` / `pico` TUI (números de línea, syntax) |
| Árbol | `tree` plano | `tree` recursivo, color, cwd por defecto |
| Logo | ASCII host | Braille U+2800 en framebuffer (`echofetch`) |
| NPU | stubs | PCI `1e7c:bca1` de verdad; sin placa no se finge |
| libc userland | — | `echlibc` (mmap BSS 64 KiB, no `malloc` de libc) |

## Decisiones que duelen (a propósito)

**Quité el escritorio de esta ISO.** El WM sigue compilable en 1.0. En 2.0
el contrato es: si hay un píxel de ventana, he fallado.

**Quité `/home`.** No es Unix. El usuario vive en `/users/<nombre>/`. HOME
apunta ahí. Quien escriba `/home` en un script 2.0 está leyendo docs de 1.0.

**`epk` no baja nada de internet.** 1.0 tenía el canal
`rogexlaboratories.com/rx-os/packages`. En 2.0 el catálogo viaja **en la
ISO**. Si no está en el medio, no existe. Prefiero un “paquete no
instalado” honesto a un curl silencioso a mi propio servidor.

**El verde cyberpunk ya no pinta toda la consola.** En 1.0 el framebuffer
usaba `RGX_FG #00FF9D` para *todo*. En 2.0 el CLI tiene paleta propia:
`night`, `matrix`, `ocean`, `amber`, `paper`. El logo Braille se ve en el
FB; en un terminal Linux de host se veía siempre.

## Lo que 2.0 añade y 1.0 no tenía

- `man` / `manual` / `apropos`
- `wdl` (download wget-like: `-O -o -c -b -L`)
- `curl -I -L -O -o -X -d -b -c -dom -H`
- `ipconf` (`ipconfig`, `ifconfig`)
- `history`, flechas arriba/abajo, Tab complete sobre `CMD_NAMES`
- caret parpadeante, rueda del ratón en scrollback
- `termtheme` aplica al CLI, no a un “desktop theme”
- `www source` (HTML crudo)
- `nmap` chico, `traceroute`
- `nano` / `pico`
- wizard TUI + `echos-install`
- memtest y recovery en GRUB
- Heap-0 como contrato de producto, no como comentario
- `doas`, `rcctl`, `pfctl`, `disklabel`, `gpt`, `dysk`, `hwprobe`
- `bench-snn`, `prisma5`
- FHS soberano (`layout.c`)
- LIVE con todos los `.epk` del catálogo *disponibles* (stubs de
  toolchain: si el ELF no cabe en RXFS, el comando existe y lo dice)

## Lo que 1.0 tiene y 2.0 no arrastra

- Dock con lupa Q16.16
- Arrastrar iconos
- Nova como ventana
- IDE gráfico
- Temas de wallpaper en el chrome
- Asistente en el escritorio (Lang/Code/Sys)

En 2.0 hubo comandos de consola para ese asistente. En 2.1 ya no
existen: el experimento se retiró entero del kernel y de la ISO.

## Numeración

| Producto | Versión | ISO |
|---|---|---|
| EchOS 1.0 Complete/Minimal/Edge | 1.0.0 | `EchOS-1.0.0-<ed>-{vm,metal}.iso` |
| echOS 2.1 Universal | 2.1.0 | `EchOS-2.1.0-universal-{vm,metal}.iso` |
| echOS 2.1 Server | 2.1.0 | misma familia, manifiesto server |

`RXOS_VERSION` en cabecera sigue diciendo `"1.0.0"` como alias legado.
No lo uses. Usa `ECHOS_VERSION`.

— R.N.
