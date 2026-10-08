# Para técnicos

Árbol, contratos, y dónde está el C. Si quieres *build*, [construir.md](construir.md).

## Forma del producto

```
boot/boot.asm          Multiboot2 trampoline
kernel/kernel.c        [echOS] status block, akida_init, RXFS
kernel/memory/heap0.*  regiones estáticas
kernel/event/          tejido SPSC (SEB)
drivers/               video, net, storage, pci, npu/akida
userland/cli/          TUI, wizard, epk, echofetch, nano, man, netcli
userland/shell/        ROSH / commands.c (dispatcher único)
userland/runtime/www.c HTTP/1.0 + TLS
drivers/net/tls_min.c  TLS 1.3 / 1.2
rogex-core/            Rust no_std (ML-KEM, ChaCha20-Poly1305)
```

Unikernel: **no hay ring-3**. `syscall` es tabla stub. Un proceso. El
“userland” es convención de carpetas.

C17 freestanding, `x86_64-none-elf`, clang, nasm, ld.lld. Sin SIMD
(`-mno-sse`). mcmodel kernel. Heap del kmalloc: 512 KiB aparte, y
**no** es Heap-0 — conviven. El cubo LIF Q6 y el bench SNN van a BSS;
el que usaba `kmalloc` era el actor del chatbot, que ya no está.
Auditoría: `docs/AUDIT_2.0.md`.

## Contratos que no rompo a la ligera

1. **Un ELF para Universal.** `make EDITION=universal`. La edición en
   disco es `/etc/echos-edition` + manifiesto aplicado.
2. **Liberation Mono.** Célula 8×16 en FB. Serial es ANSI.
3. **CLI_ONLY.** `ECHOS_HAS_DESKTOP=0`. `fb_clear` de consola usa
   `g_con_bg`, no el verde RGX.
4. **`epk` offline.** Catálogo ISO `/media/epk` + mirror in-kernel.
5. **CMD_NAMES es la verdad.** Tab complete, `help` y el dispatcher
   leen el mismo array. Si añades un comando y no lo pones ahí, no
   existe para el usuario.

## Dispatcher

`userland/shell/commands.c` → `commands_dispatch`. `line` se tokeniza
in-place (`split3`). Salida por `o_str` a un buffer de 8192 (`g_term`).
`man curl` cabe; un HTML de 192 KiB no: `curl -o` al RXFS.

Comandos nuevos 2.0 viven en:

| Fichero | Qué |
|---|---|
| `userland/cli/man.c` | páginas |
| `userland/cli/netcli.c` | curl / wdl / wget / ipconf |
| `userland/cli/nano.c` | editor |
| `userland/cli/wizard.c` | instalador |
| `userland/cli/epk.c` | paquetes |
| `userland/cli/tui.c` | 80×25, teclas, serial |

## Boot

GRUB `boot/grub/grub-universal.cfg`:

- `echos.mode=live` — wizard + LIVE
- `echos.mode=memtest`
- `echos.mode=recovery`

`userland/cli/cli.c` `echos2_boot()`. Si `wizard_autostart()`, TUI.
`q` → `commit_live()` → `live_enter()`.

virtio-blk: Command Interrupt Disable (bit 10). Sin eso, IRQ compartida
con virtio-net y el boot se queda en `[echOS] nic…`. Polled I/O. Lo
aprendí la mala.

## Red (capa a capa)

```
netif (virtio-net | e1000 | r8169 | rtl8139)
  → ipstack (ARP, ICMP, UDP, DHCP, traceroute)
    → tcp.c  (un socket cliente)
      → tls_min.c
        → www.c  HTTP/1.0
          → netcli.c  curl/wdl
```

TLS ClientHello ofrece `0x1301` luego `0xC02F`. Extensiones:
`supported_versions`, `key_share` P-256, `psk_key_exchange_modes`.
Dummy CCS post-SH (middlebox). HKDF-SHA256. GCM 12-byte IV XOR seq.
Transcript de `s ap traffic` **corta en server Finished**; `c ap` incluye
Client Finished. Si mezclas los hashes, el handshake “OK” y el GET https
vuelve vacío. Ya me pasó.

Sin pin de CA. Leaf RSA se usa en 1.2 para el ServerKeyExchange.

## FS

RXFS: 64 inodos, 64 KiB cada uno (esta generación). `vfs_*` encima.
`layout.c` siembra el árbol 2.0. `HOME=/users/<user>`.

## IRQ / cooperativa

PIT 100 Hz. `sched_poll_relax` en drivers. Un hilo. NIC puede ser IRQ
(virtio) o poll (e1000). El tejido de eventos (`kernel/event`) es SPSC:
el ISR produce, el idle consume. Determinista: el umbral LIF **no**
gatea caminos de corrección (tecla, RX). Eso es del manifiesto viejo y
sigue vigente.

## Cosas que vas a odiar (justas)

- Un TCP a la vez.
- HTTP/1.0, `Connection: close`.
- `g_term[8192]` se come `help` largo; por eso existe `man`.
- POST grande: el buffer de request es 1400; el resto va en chunks.
  Form corto sí. Un JSON de 80 KiB, reza.
- Signed char vs UTF-8: ya no comemos el em-dash, pero no soy glibc.

## Puntos de entrada útiles

```
commands_dispatch     userland/shell/commands.c
www_http_request_ex   userland/runtime/www.c
tls_handshake         drivers/net/tls_min.c
epk_cmd               userland/cli/epk.c
console_set_theme     drivers/video/terminal.c
fb_putc_braille       drivers/video/framebuffer.c
akida_init            drivers/npu/akida.c
heap0_init            kernel/memory/heap0.c
```

— R.N.
