# Neuromórfico en 2.1

El OS *siente* eventos, no un `while(1) poll`. Eso viene de rxOS.
2.1 no lo inventa: lo deja a la vista, sin desktop que lo disimule.
Esta ISO es la casa de software neuromórfico y robótica. Echo AI (host)
no va dentro.

Manifiesto largo (laboratorio, no producto): `docs/neuromorphic_manifesto.md`.

## Tres capas, no mezclarlas

1. **Tejido de eventos del kernel** — SPSC, actores LIF, idle = HLT.
   Siempre on. Teclado y RX no dependen de umbral.
2. **SNN de aplicación** — PRISMA 5, `bench-snn`, cubo Q6 (LIF entero
   de 64 celdas, `kernel/event/snn_q6.c`) en BSS.
3. **Silicio** — Akida AKD1000 si PCI `1e7c:bca1`. Si no, LIF en
   x86_64 y se imprime `absent`.

Loihi no es un backend de 2.1. No hay driver. `neurocpu loihi` se niega
y no sale en `help`.

## Comandos

```text
hwprobe                 # PCI + Akida + discos + NIC
neurocpu                # software | akida
neurocpu software
bench-snn
prisma5
prisma5 alpha
prisma5 null
prisma5 stress          # 64 ráfagas sintéticas del cubo Q6
```

`bench-snn` suelta sparsity, latencia, mW, delta Heap-0. Úsalo si
vas a decir “números”.

## Akida

Driver: `drivers/npu/akida.c`. Probe PCI, BAR0 1 MiB, magic `AKID`.
Registros de potencia y latencia son **contrato EchOS**, no el
runtime propietario de BrainChip. No hay MetaTF aquí. No lo voy a
vincular.

El driver de Akida es **sólo detección**: dice si la tarjeta está en el
bus PCI y dónde dejó el firmware su BAR0. Ya no declara registros
`POWER_MW` / `TEMP_C` / `LAT_NS`: esos offsets nos los habíamos inventado,
así que un milivatio leído de ahí habría sido una medición fabricada.
Sin contrato verificado no hay registros, y sin placa no hay nada que leer.

## El chatbot que hubo

Hubo un chatbot en el kernel: pesos por módulo GRUB, léxico, generadores
de frases y una ventana de chat en el escritorio. Se retiró entero — del
ELF, de la ISO, del menú de GRUB y de la documentación — y sus fuentes
viven en `legacy/kernel-navi/`. No se sustituye por otro: este unikernel
no aloja modelos de lenguaje.

Lo que se quedó es lo que sirve sin él: el tejido de eventos, el SEB, el
cubo LIF Q6 y el marco WSP de 16 bytes como transporte. La próxima mente
(Echo AI) es otro árbol, en el host, y esta ISO no la embebe.

## PRISMA 5

Drive **sintético** del cubo Q6 (no hardware EEG). Stress 64. Sin
deadlock (o es bug).
No publiques esto como validación clínica.

## Energía

Idle: `HLT` (C1). ACPI si el firmware (BOCHS/QEMU) expone C2/C3;
latencias absurdas en QEMU no las uses como paper. RAPL: Intel
real; en QEMU Virtual CPU suele #GP y el guard lo atrapa.
`power` lo dice.

La historia “neuromórfico = 0 vatios” es falsa. La historia
“no hay busy loop de userspace porque no hay userspace” es
literalmente el unikernel.

## Honestidad para citas

El software LIF está. El evento-tejido está. El probe Akida está.
El crossbar analógico **no**. Si un paper necesita memristor
físico, este repo no es el dataset.

— R.N.
