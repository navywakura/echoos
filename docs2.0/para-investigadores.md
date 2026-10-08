# Para investigadores

Qué hay aquí que no sea un toy OS con prompt bonito. Y qué **no** hay,
que es igual de importante.

## Posición

echOS 2.1 es un unikernel von Neumann (x86_64, reloj, HLT). Encima corre
un **modelo software** de computación por eventos y un SNN LIF / STDP
entero. El hardware neuromórfico (Akida AKD1000, Loihi) es *objetivo de
I+D* cuando el silicio está en el bus; si no está, el backend es LIF en
CPU y se dice.

Regla del laboratorio, copiada de mi propio manifiesto y que no he
relajado:

- **VALIDADO (software):** medible en QEMU o metal.
- **OBJETIVO DE I+D (hardware):** memristores, crossbar, in-memory.
  No está embarcado.
- **x86 sigue siendo x86.** “Consumo cero” nunca es termodinámico. Es
  *ausencia de busy-loop en el idle*.

Paper de base (1.x, sigue aplicando el modelo mental):
`docs/neuromorphic_manifesto.md`, `docs/paper/`.

## Heap-0 como objeto de estudio

Layout estático, O(1), cero fragmentación por diseño. Tabla de regiones
con nombre, base, tamaño, propósito. Pool WSP 64 KiB. Device manager 64
entradas.

**No** es un allocador general. `kmalloc` (512 KiB) sigue en el árbol
para código que no se ha migrado. El cubo LIF (`snn_q6_t`) está en BSS.
Si publicas números de “cero malloc”, mira el camino concreto:
`bench-snn` y `prisma5` sí lo cumplen.

`epk stress` mide `heap_used` antes/después de install/remove. El
delta tiene que volver.

## Tejido de eventos

SPSC ring, eventos tipados (`kernel/event/`). Actores LIF Q16.16. El
ISR de virtio-net puede producir `RX_EVENT_NET_RX_READY`. El idle mete
un deadline de 1 tick PIT para e1000 (sin IRQ).

Umbral de spike **no** silencia teclado ni RX. Corrección ≠ estética.
Eso está escrito en el header de `netif.h` y lo mantengo.

## PRISMA 5

Pipeline EEG → spikes. Comandos: `prisma5`, `prisma5 stress`, `bench-snn`.
Métricas: sparsity, latencia µs, mW (estimación), Heap-0 delta. 64
ráfagas en stress. La red (`g_net`, `g_eeg`) vive en BSS; no en el
stack de un `rx_actor_t` de 16 KiB (eso ya reventó una vez).

No es un EEG clínico. Es un banco reproducible *dentro* del OS.

## Akida AKD1000

PCI vendor `0x1E7C` device `0xBCA1`. BAR0 1 MiB, preferido `0xFED00000`
si el firmware no programó. Mapa MMIO **nuestro** (`AKID` magic), no
una copia de MetaTF:

| Off | Registro |
|---|---|
| 0x00 | MAGIC |
| 0x10 | POWER_MW  (presupuesto < 100 mW en pico de inferencia) |
| 0x14 | TEMP_C |
| 0x20 | LAT_NS |

Sin placa: `akida_present() == false`. `neurocpu akida` falla. Software
LIF. No hay blob BrainChip en este árbol. No lo voy a meter.

## RogexWSP

16 bytes fijos: versión, opcode, nodo origen, nodo destino, secuencia
big-endian y 8 bytes de carga. Es transporte, no lengua: enruta bytes
por el bus de spikes (`SEB_CH_WSP`) y no significa nada por su cuenta.
`wsp-tool inject | decode | loopback` es la superficie.

El paquete semántico anterior (átomos, seis ejes de afecto, tabla de
generadores) existía para el chatbot y salió con él a
`legacy/kernel-navi/`.

## Criptografía

`rogex-core` Rust `no_std`:

- ML-KEM-768 (FIPS 203) — handshake self-test en boot
- ChaCha20-Poly1305 (KAT RFC 8439 en boot)
- SHA-3

TLS del cliente web: C, AES-128-GCM, P-256, SHA-256, HKDF para 1.3.
**Sin auditoría externa.** Se dice tal cual. Sin pin de CA.

ROGEX-PQC en Server es ese núcleo, no un sshd post-cuántico mágico.

## Red como instrumento

Un socket TCP. HTTP/1.0. Suficiente para medir handshake TLS 1.3 contra
Cloudflare (example.com: handshake OK en QEMU+slirp, 2026-08-26).
No es un stack para paper de datacenter. Es un cliente honesto.

## Reproducir números

```text
hwprobe          PCI, discos, Akida, NIC
bench-snn        sparsity / us / mW / Heap-0
prisma5 stress   64 ráfagas
epk stress [N]   install/remove
echofetch        tarjeta
ipconf           MAC/IP
tls example.com  versión negociada
```

QEMU: `q35`, KVM, virtio-net, 512 MiB. ISO
`build/EchOS-2.0.0-universal-vm.iso`. LIVE: `q` en el idioma.

## Qué no cites como resultado

- “echOS corre en Akida” — corre *con* Akida si el PCI aparece.
- “TLS 1.3 completo nivel navegador” — handshake sí; app-data https
  todavía puede fallar. HTTP claro sí.
- “doas es seguridad” — un anillo.
- “RXFS es un filesystem de producción” — 64 slots.
- NVMe: clase PCI reconocida, **sin driver**. AHCI/IDE/virtio-blk sí.

Si vas a citar, cita el comando y la fecha del log. No el slogan.

— R.N.
