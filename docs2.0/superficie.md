# Superficie pública — estado de comandos (echOS 2.1.0-honest)

Cada nombre que un extraño puede teclear o leer en `help` / `epk list` /
`man` lleva una etiqueta:

- **REAL** — el unikernel hace ese trabajo.
- **NOTA** — el nombre existe, el trabajo es un flag, un dump, o un
  re-dispatch. El texto lo dice.
- **AUSENTE** — fuera de Tab y de `help`. Si alguien lo escribe: *not in
  this unikernel* o `command not found`.

Fuente de Tab: `CMD_NAMES` en `userland/shell/commands.c`.
Fuente de paquetes públicos: `epk list` (no `--lab`).

## Comandos (`CMD_NAMES`)

| Comando | Etiqueta | Qué hace |
|---------|----------|----------|
| help about status mem limits uptime power bench devices env | REAL | kernel / benchmark; `status` nombra la firmware del handoff |
| clear cls reboot halt | REAL | consola / reset |
| ls cd pwd cat write rm cp mv mkdir rmdir tree | REAL | RXFS |
| head tail less grep find du df nano history | REAL | RXFS (less no es GNU less) |
| date tz kbd termtheme | REAL | reloj, mapa, paleta |
| man apropos | REAL | páginas embebidas |
| echofetch hwprobe live whoami uname hostname echo | REAL | identidad / PCI |
| www curl wget dns tls ping nics ipconf trace nmap | REAL | IPv4; HTTPS puede cuerpo vacío |
| wired chat say | REAL | L2 0x88B5 |
| epk echos-install install save load format partition cfdisk | REAL | RXFS / disco; `partition <d> gpt` escribe GPT y la re-verifica |
| gpt | NOTA | writer GPT **sin CRC32 válido** (se dice) |
| doas | NOTA | re-dispatch; un espacio de direcciones; no aísla |
| rcctl | NOTA | flags `/etc/rc.conf.local`; no hay daemons |
| pfctl | NOTA | boolean RAM + dump de `/etc/pf.conf`; no filtra |
| sudoers | REAL | imprime `/etc/sudoers` |
| neuro neurocpu prisma5 bench-snn | REAL | SNN in-kernel; Akida = sonda PCI |
| top rxmon capture verbose | REAL | tarjeta de tareas / BMP / traza `#` |
| alias (catalog, wander, unveil, …) | REAL | mismos binarios, otro nombre |

## Paquetes `epk list` (público)

| Paquete | Etiqueta | Descripción |
|---------|----------|-------------|
| echos-base | REAL | kernel + Heap-0 + ROSH |
| echos-shell | REAL | doas/rcctl/pfctl como flags |
| echofetch | REAL | tarjeta |
| echos-diag | REAL | memtest / prisma5 / bench-snn |
| echos-net | REAL | pila IPv4 cliente |
| echos-npu | REAL | sonda Akida; LIF si no hay placa |
| echos-io | REAL | nota de drivers ya en el kernel |
| echos-akida | REAL | reserva PCI 1e7c:bca1 |

`epk install` escribe un **texto** en RXFS, no un ELF.

## `epk list --lab` (host / flags)

Todos **NOTA**. No arrancan un proceso.

tcc, make, python314, quickjs, nasm, rustc-echos, epk-build, echos-dev,
echos-cli, echos-httpd, echos-ftpd, echos-sshd, echos-pqc, rg, fd, bat,
eza, z, unp, dust, duf, hyperfine, localsend, rx-pkg.

## Ausentes explícitos

| Nombre | Por qué |
|--------|---------|
| tcc / python / git / rustc in-OS | toolchain de **host** + echlibc |
| sshd / httpd / ftpd reales | un TCP cliente ≠ daemon |
| chatbot / LLM / echo-in-OS | experimento cerrado y fuera del ELF y de la ISO |
| browse / nova | escritorio 1.0. 2.1 no es un navegador |
| loihi | cero interfaz en este árbol |
| htop / tmux / split panes | una consola |
| Wi-Fi | PCI list; cero driver |
| aplicación UEFI nativa | arranca bajo OVMF vía GRUB EFI, no como app EFI propia |
| inteligencia que decida | el productor de intenciones es un reflejo de placeholder |
| armar / despegar | el puente no arma: pide modo y envía consignas, nada más |
| MAVLink por UART | sólo UDP; el serie es trabajo pendiente |
| NVMe 4 Kn, varias colas, IRQ | una cola, polled, bloques de 512 B |
| uso comercial / “surf the web” | investigación, robótica, SNN |

Si un README, un `help` o un `epk list` contradice esta página, gana
esta página y `docs2.0/limites.md`.
