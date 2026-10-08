# Heap-0

Nombre de marketing para una idea simple: **la memoria crítica se
reserva en compile time**. BSS. Tabla. O(1). Sin fragmentar.

Ficheros: `kernel/memory/heap0.c`, `heap0.h`.

## Por qué

Un unikernel que malloc-ea en el IRQ de la NIC o en el spike path
es un chiste. Heap-0 es el contrato: I/O, WSP, device manager,
iconos (aunque 2.0 no pinte iconos de desktop) tienen sitio fijo.

`heap0_init()` registra regiones. `heap0_get_region(i)` te da
nombre, base, tamaño, propósito. `echofetch` / `mem` las enseñan.

## Números (esta generación)

| Región | Tamaño | Para |
|---|---|---|
| WSP pool | 64 KiB | mensajes RogexWSP |
| device manager | 64 slots | `devices` |
| (histórico) icon cache | 16 | 1.0; el layout sigue |

El backbuffer de framebuffer **no** es Heap-0 puro: es un mapeo LFB
+ copia. Cuenta en el presupuesto de RAM (3.6 MiB a 1280×720×32).
Edge con 16 MiB: usa texto GRUB.

## Qué no es Heap-0

- `kernel/memory/heap.c` — `kmalloc` / `kfree`, arena 512 KiB.
  Sigue. El que bebía de ahí (el actor del chatbot) ya no está en el
  árbol activo, pero la arena no se ha retirado.
- PMM — frames físicos Multiboot2.
- VMM — páginas 4K para MMIO / LFB.
- RXFS — slots, no heap.
- `echlibc` mmap — 64 KiB BSS para user ELF, otro contrato.

Si mides “cero malloc” en un paper, acota el camino: `bench-snn` y
`prisma5` en BSS. No el kernel entero.

## Determinismo

O(1) significa: no hay first-fit, no hay coalescing, no hay “el
tercer install de epk falla porque el heap está rayado”. `epk stress`
tiene que devolver el mismo `heap_used`. Si no, es bug.

Fragmentación cero *por construcción*, no por compactar.

## Relación con el SNN

Los buffers grandes (`g_net`, `g_eeg`, pesos que caben) van a BSS
o a módulo GRUB. Un `rx_actor_t` de 16 KiB en stack ya nos dio un
#GP. No se vuelve a poner una red en el stack. Punto.

## Verlo

```text
mem
echofetch
hwprobe
```

Código: `heap0_region_t`. Si añades una región, la tabla. Si la
escondes en un `static uint8_t buf[1<<20]` suelto, has mentido al
contrato aunque el binario arranque.

— R.N.
