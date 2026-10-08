# Límites — lo que no hago

Política del laboratorio: si no está, se dice. Esta página es tan
producto como `echofetch`.

## Hardware

| Cosa | Estado |
|---|---|
| WiFi | no. PCI se lista. Cero driver. |
| NVMe | driver real: reset, colas admin, Identify, 1 cola de E/S, lectura/escritura. Un sector por comando, polled, sin IRQ ni MSI-X. Namespace 4 Kn no soportado. |
| UEFI nativo | el kernel **no** es una aplicación UEFI. La ISO híbrida arranca bajo OVMF porque GRUB EFI la carga por Multiboot2; el handoff dice cuál de las dos firmwares fue. |
| USB HID | no como input de diario. PS/2 teclado y ratón. |
| Audio | no en 2.1 CLI. |
| GPU accel | no. LFB. |
| Akida | probe PCI. Sin placa = software LIF. Sin blob BrainChip. |
| Loihi | fuera de 2.1. Cero interfaz. |
| ARM64 kernel | el árbol genérico ya no toca la máquina (`make portability`), pero **nadie lo ha compilado para AArch64**. Eso es la fase G. |

## Software

| Cosa | Estado |
|---|---|
| Multi-process / ring-3 | stub. Un task. |
| SMP real | QEMU puede `-smp`; el scheduler no es un Linux. |
| POSIX | no. Alias de comandos, no glibc. |
| JS en `-dom` | no hay VM. Espera + refetch. |
| TLS 1.3 app-data | handshake OK; GET https puede volver vacío. HTTP sí. |
| Pin de CA | no. |
| IPv6 | no. |
| Varios TCP a la vez | no. |
| sshd/httpd/ftpd reales | flags `rcctl`. Server manifiesto. |
| `doas` aislamiento | no. Un mapa de memoria. |
| RXFS grande | 64 × 64 KiB. |
| Compiladores en LIVE | ausentes. `epk list --lab` = notas de host. |
| Pager `less` GNU | comando propio, consola 8192. |
| Locale completo | wizard ES/EN. Comandos en inglés. |
| Persistencia LIVE | no, a propósito. |
| Navegador / JS | no. `curl` baja bytes. |
| Uso comercial como OS general | no. Laboratorio / robótica / SNN. |

## Runtime robótico

Existe el camino sensor → runtime → intents → gate → transporte, con cuatro
colas estáticas y sus contadores (`robot`). Lo que **no** existe todavía:

| Cosa | Estado |
|---|---|
| Productor de intenciones | reflejo de placeholder: distancia, batería y enlace. No planifica, no recuerda, no aprende. |
| Transporte | MAVLink 2 sobre UDP a un PX4 real (`px4 start`). Sin enlace, la cola de salida se llena y descarta. UART todavía no. |
| Sensores reales | telemetría de PX4 (actitud, posición, batería, distancia). Ningún sensor propio de esta placa. |
| Actuadores | el ABI de intención **no puede** expresar PWM, canal ni servo. Deliberado. |
| Geofence | caja alineada a ejes, sin altura de terreno ni polígonos. |

## Techos

El binario los dice él mismo: `limits` en la consola los imprime desde
las mismas constantes que usan los subsistemas, no desde una tabla
escrita a mano al lado.

| Techo | Valor |
|---|---|
| tareas | 4 |
| pila por tarea | 65536 B (los IRQ corren en ella) |
| anillo SEB | 1024 spikes, 8 canales |
| paquete WSP | 16 B |
| pool WSP de Heap-0 | 65536 B de BSS |
| dispositivos | 64 slots |
| arena kmalloc | 524288 B |
| RXFS | 64 ficheros x 65536 B, 64 dirs, profundidad 8 |
| ramdisk | 524288 B |

`make test-universal-x86_64` compara esta tabla con lo que imprime el
guest. Si una constante se mueve y esta página no, el test se pone rojo.
Que el kernel *rechace* al llegar al techo es otra prueba; esta sólo
demuestra que binario y documento cuentan lo mismo.

## Heap-0

El contrato **no** se cumple en todo el kernel. `kmalloc` vive: la
arena de 512 KiB sigue ahí y `limits` la imprime. Está en
`docs/AUDIT_2.0.md`. No lo voy a borrar de la auditoría para que
Heap-0 quede “limpio” en un README.

## Red

Un hilo cooperativa. TX síncrono. RXFS no es IRQ-safe: un IRQ de
NIC durante `vfs_write` es el riesgo. Bajo, no cero.

virtio-blk INTx se enmascara porque compartía línea con la NIC y
el boot moría. Polled. Correcto, no elegante.

## Criptografía

`rogex-core` no tiene auditoría de terceros. KAT en boot ≠ pentest.
ML-KEM self-test ≠ “somos post-cuánticos en producción”.

## 1.0

El escritorio Eclipse **no está abandonado**; no está *en esta ISO*.
Si un usuario 1.0 abre 2.1 y pregunta dónde está el dock, la
respuesta es esta carpeta, no un parche.

## Lo que sí firmo

- El banner de boot no imprime `OK` si esa etapa no se comprobó.
- No se finge un NIC, un NPU ni un daemon.
- `command not found` ≠ `not installed`.
- Las páginas `man` mienten menos que un `--help` de dos líneas.

Si encuentras un `OK` mentiroso, es un bug de producto, no de
copy.

— R.N.
