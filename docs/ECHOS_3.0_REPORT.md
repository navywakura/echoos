# echOS 3.0.0 — informe de evidencia

Generado por `tools/report30.py`. Cada número lo produjo la propia
máquina con su orden `report`; esta herramienta sólo recoge, hashea y
formatea. Sin direcciones, sin timestamps, sin rutas del host.

## Artefactos

| Artefacto | sha256 |
|---|---|
| `EchOS-3.0.0-universal-vm.iso` | `665971e1bce317cec34e81c754435328dc4f50210cb52c72f7ff16f71c3260ed` |
| `rxos.elf (x86_64)` | `fe28b51baa119ead745adde0202c45e902182c3aacfbe0193386e597a6442471` |
| `rxos.img (aarch64)` | `4a251b9580db1ee56fd6e5e0295118758309e89678edfb1adddbe7b9f7b8b0da` |

## x86_64 · BIOS (Multiboot2/GRUB)

| Campo | Valor |
|---|---|
| versión | 3.0.0 |
| arquitectura | x86_64 |
| boot mode | multiboot2 · firmware bios |
| memoria total | 511 MiB |
| memoria física entregada | 3704 KiB |
| resto en la región del asignador | 490 MiB (no es «RAM libre»: el resto es RAM que el asignador no eligió) |
| heap dinámico usado | 0 B de 524288 B |
| Heap-0 reservado estáticamente | 3850752 B en 5 regiones |
| **allocations en sensor→intent** | **0** |
| latencia p50 | < 0 µs |
| latencia p95 | < 0 µs |
| latencia p99 | < 0 µs |
| jitter (p95 − p50) | 0 µs |
| balance de registros | 61 inyectados = 50 aceptados + 3 rechazados + 8 descartados |
| safety gate | 48 vistos = 43 aceptados + 4 MODIFY + 1 BLOCK |
| expiraciones | 1 |
| deadlines incumplidos | 1 (la cola del gate registra el mismo evento: 1) |
| watchdog (intenciones emitidas) | 1 |
| guardas de pila intactas | sí |
| enlace MAVLink | arriba |
| tramas rx/tx | 636 / 189 |
| COMMAND_ACK | 36 |
| **pérdida de paquetes** | **0** (0 huecos de secuencia) |
| errores CRC | 0 |
| caídas de enlace | 0 |
| mensajes no decodificables | 202 (secuencia seguida igualmente) |

Colas (`pushed / popped / drops / expired / high water / capacidad`):

| Cola | Valores |
|---|---|
| `sensor` | pushed 805 popped 805 drops 8 expired 0 high_water 32 capacity 32 |
| `intent` | pushed 184 popped 184 drops 0 expired 0 high_water 1 capacity 16 |
| `gate` | pushed 186 popped 186 drops 0 expired 1 high_water 1 capacity 16 |
| `out` | pushed 186 popped 186 drops 1 expired 0 high_water 16 capacity 16 |

Medidas de esta ejecución, **no** reproducibles entre corridas:

- latencia bruta min/media/max: 0 / 54 / 10000 µs
- muestras de latencia acumuladas: 185

**Prueba de pérdida de enlace** — lo dice PX4, no nosotros:

```
WARN  [commander] Connection to mission computer lost
```

## x86_64 · UEFI (OVMF)

| Campo | Valor |
|---|---|
| versión | 3.0.0 |
| arquitectura | x86_64 |
| boot mode | multiboot2 · firmware uefi |
| memoria total | 506 MiB |
| memoria física entregada | 3716 KiB |
| resto en la región del asignador | 482 MiB (no es «RAM libre»: el resto es RAM que el asignador no eligió) |
| heap dinámico usado | 0 B de 524288 B |
| Heap-0 reservado estáticamente | 3850752 B en 5 regiones |
| **allocations en sensor→intent** | **0** |
| latencia p50 | < 31 µs |
| latencia p95 | < 127 µs |
| latencia p99 | < 127 µs |
| jitter (p95 − p50) | 96 µs |
| balance de registros | 61 inyectados = 50 aceptados + 3 rechazados + 8 descartados |
| safety gate | 48 vistos = 43 aceptados + 4 MODIFY + 1 BLOCK |
| expiraciones | 1 |
| deadlines incumplidos | 1 (la cola del gate registra el mismo evento: 1) |
| watchdog (intenciones emitidas) | 0 |
| guardas de pila intactas | sí |

Colas (`pushed / popped / drops / expired / high water / capacidad`):

| Cola | Valores |
|---|---|
| `sensor` | pushed 50 popped 50 drops 8 expired 0 high_water 32 capacity 32 |
| `intent` | pushed 46 popped 46 drops 0 expired 0 high_water 1 capacity 16 |
| `gate` | pushed 48 popped 48 drops 0 expired 1 high_water 1 capacity 16 |
| `out` | pushed 47 popped 47 drops 1 expired 0 high_water 16 capacity 16 |

Medidas de esta ejecución, **no** reproducibles entre corridas:

- latencia bruta min/media/max: 5 / 32 / 124 µs
- muestras de latencia acumuladas: 47

## AArch64 · QEMU virt (device tree)

| Campo | Valor |
|---|---|
| versión | 3.0.0 |
| arquitectura | aarch64 |
| boot mode | device-tree · firmware unknown |
| memoria total | 512 MiB |
| memoria física entregada | 0 KiB |
| resto en la región del asignador | 383 MiB (no es «RAM libre»: el resto es RAM que el asignador no eligió) |
| heap dinámico usado | 0 B de 524288 B |
| Heap-0 reservado estáticamente | 3850752 B en 5 regiones |
| **allocations en sensor→intent** | **0** |
| latencia p50 | < 31 µs |
| latencia p95 | < 127 µs |
| latencia p99 | < 255 µs |
| jitter (p95 − p50) | 96 µs |
| balance de registros | 61 inyectados = 50 aceptados + 3 rechazados + 8 descartados |
| safety gate | 48 vistos = 43 aceptados + 4 MODIFY + 1 BLOCK |
| expiraciones | 1 |
| deadlines incumplidos | 1 (la cola del gate registra el mismo evento: 1) |
| watchdog (intenciones emitidas) | 1 |
| guardas de pila intactas | sí |
| enlace MAVLink | arriba |
| tramas rx/tx | 498 / 176 |
| COMMAND_ACK | 38 |
| **pérdida de paquetes** | **0** (0 huecos de secuencia) |
| errores CRC | 0 |
| caídas de enlace | 0 |
| mensajes no decodificables | 194 (secuencia seguida igualmente) |

Colas (`pushed / popped / drops / expired / high water / capacidad`):

| Cola | Valores |
|---|---|
| `sensor` | pushed 649 popped 649 drops 8 expired 0 high_water 32 capacity 32 |
| `intent` | pushed 172 popped 172 drops 0 expired 0 high_water 1 capacity 16 |
| `gate` | pushed 174 popped 174 drops 0 expired 1 high_water 1 capacity 16 |
| `out` | pushed 174 popped 174 drops 1 expired 0 high_water 16 capacity 16 |

Medidas de esta ejecución, **no** reproducibles entre corridas:

- latencia bruta min/media/max: 4 / 38 / 169 µs
- muestras de latencia acumuladas: 173

**Prueba de pérdida de enlace** — lo dice PX4, no nosotros:

```
WARN  [commander] Connection to mission computer lost
```

## Reproducibilidad

Dos arranques del mismo artefacto AArch64: **idénticos** en los 36 campos declarados deterministas —
versión, mapa de memoria, allocations, balance de registros, veredictos
del gate, colas con su marca de agua, guardas de pila.

**La latencia no está en ese conjunto, y es a propósito.** La primera
versión de este informe la daba por reproducible porque los percentiles
son cotas de bucket (potencias de dos). Dos corridas la desmintieron: el
p50 saltó un bucket. Bajo TCG la distribución cae a caballo de un límite,
así que el número se mueve sin que se mueva nada del sistema. En vez de
ensanchar la comprobación hasta que pase, se publican las dos medidas:

| Campo | Corrida 1 | Corrida 2 |
|---|---|---|
| `latency_p50_us_lt` | 63 µs | 255 µs |
| `latency_p95_us_lt` | 127 µs | 255 µs |
| `latency_p99_us_lt` | 127 µs | 255 µs |
| `latency_jitter_us_lt` | 64 µs | 0 µs |

## Cumplimiento del §7, línea por línea

| Bloque | Requisito | Estado | Evidencia |
|---|---|---|---|
| Identidad | Versión 3.0.0 | ✅ | las tres placas la declaran |
| Identidad | Producto: robotic edge unikernel | ✅ | README y `about` describen el pipeline, no un chatbot |
| Identidad | Sin promesas de chatbot/LLM/SLM/NAVI/Akida | ✅ | el eslogan compilado ya no cita Akida; CERT-21 comprueba el ISO byte a byte contra los nombres retirados |
| Identidad | `echoai/` intacto | ✅ | no se ha tocado en ninguna fase |
| Arquitecturas | x86_64 BIOS arranca | ✅ | 130 comprobaciones |
| Arquitecturas | x86_64 UEFI arranca en QEMU | ✅ | 130 comprobaciones |
| Arquitecturas | AArch64 **UEFI** arranca en QEMU virt | ✅ | 60 comprobaciones: el firmware edk2 carga la misma imagen como aplicación PE y el mapa de memoria sale del firmware, no de una suposición |
| Arquitecturas | Al menos una arquitectura completa la demo | ✅ | la completan x86_bios, aarch64 |
| Build | Artefactos separados por arquitectura y edición | ✅ | `build/<arch>/<edition>/` |
| Build | Builds limpios reproducibles | ✅ | `make repro-universal-x86_64` y dos builds ARM con el mismo sha256 |
| Memoria | Cero allocations dinámicas sensor→intent | ✅ | contador de llamadas a kmalloc alrededor del escenario |
| Memoria | High-water marks medidos | ✅ | columna `high_water` de las cuatro colas |
| Memoria | Drops y expiraciones medidos | ✅ | columnas `drops` y `expired` |
| Memoria | Watchdog funcional | ✅ | habló solo en x86_bios, aarch64 |
| Almacenamiento | GPT, NVMe, persistencia tras reboot, errores y timeouts | ✅ | 20 comprobaciones |
| Robótica | Telemetría real, intención enviada, PX4 sigue siendo el controlador | ✅ | 25 comprobaciones |
| Robótica | Mensajes corruptos/fuera de rango rechazados | ✅ | CRC, secuencia, sysid/compid y ABI de sensores; pérdida medida 0 en x86_bios, aarch64 |
| Robótica | Pérdida de enlace ⇒ conducta segura | ✅ | PX4 lo registra en x86_bios, aarch64 |
| Robótica | Ninguna interfaz controla PWM/motores | ✅ | el puente sólo emite intención de alto nivel; no hay mensaje de actuación en la tabla MAVLink |

Sin requisitos pendientes en esta tabla.

## Certificados citados

| Certificado | Resultado |
|---|---|
| CERT-21 · BIOS | verde · 130 comprobaciones |
| CERT-21 · UEFI | verde · 130 comprobaciones |
| CERT-22 · storage | verde · 20 comprobaciones |
| CERT-E · PX4 x86_64 | verde · 25 comprobaciones |
| CERT-G · AArch64 | verde · 76 comprobaciones |
| CERT-G · AArch64 UEFI | verde · 60 comprobaciones |

## Capacidades ausentes

- **Sin device tree bajo UEFI**: el firmware edk2 de QEMU publica ACPI y no una tabla FDT, así que por esa vía el mapa de memoria viene del propio firmware. echOS no lee ACPI: si un día necesita algo que sólo esté ahí, habrá que escribir ese lector.
- **Sin framebuffer en AArch64**: la consola es PL011 serie. La fuente JetBrains Mono, los paneles y el scroll son la CLI de x86.
- **Sin userland completo en AArch64**: shell de diagnóstico de nueve órdenes. Sin paquetes epk, sin TLS, sin instalador.
- **Tres direcciones fijas en AArch64** (PL011, GIC, virtio-mmio): el DTB ya se lee para la memoria, enumerar periféricos desde él está pendiente.
- **Sin SMP** en ninguna arquitectura: un núcleo.
- **Akida**: sonda PCI que se declara *detectada, no soportada*. Nada se descarga en ella y no es un requisito.
- **Sin chatbot, LLM ni SLM**. El tejido de eventos spiking (LIF/Q6) es real y es lo único neuromórfico que hay.
- **PX4 SITL es una herramienta del host**: no está en la ISO ni lo estará.
