# Para usuarios

Si no programas, esto es lo que necesitas.

## Qué vas a ver

Arrancas la ISO. GRUB espera dos segundos y entra en **Installer / LIVE**.
Sale un menú TUI: idioma, zona, teclado, edición. Abajo pone que **q** es
LIVE. Si solo quieres mirar, pulsa `q`. Nada se escribe a disco.

Entras en una consola negra. El prompt parece:

```text
live@echos /users/live#
```

Eso es todo. No hay escritorio. No hay iconos. Si eso te asusta, 1.0
Complete es tu ISO.

## Lo primero que te dejo probar

```text
echofetch
help
man
man curl
ipconf
tree
```

`echofetch` pinta el logo a la izquierda y las specs a la derecha.
El arte es de uso libre.

## Archivos

No hay Explorador. Hay comandos. Los de siempre, más los nombres que
yo preferí (BSD, no clones de GNU):

| Quieres | Escribes |
|---|---|
| listar | `ls` o `catalog` |
| entrar | `cd` o `wander` |
| dónde estoy | `pwd` o `whereami` |
| leer | `cat` o `unveil` |
| escribir un editor | `nano notas.txt` |
| árbol | `tree`  (o `tree /` para todo) |
| borrar | `rm` |

Tu carpeta es `/users/live/` (o el nombre que hayas puesto al instalar).
Ahí: Desktop, Documents, Downloads… según el idioma del wizard.

**RXFS es pequeño.** Un fichero son 64 KiB. No vas a guardar una película.
Es un laboratorio, no un NAS.

## Red

```text
www on
ipconf
ping 10.0.2.2
curl -I http://example.com
wdl https://example.com
```

`www on` enciende IPv4. En QEMU suele caer en `10.0.2.15`. En un portátil
hace falta cable Ethernet (virtio, e1000, Realtek 810/8139). **No hay
WiFi.** El RTL8188EE del HP 15 se ve en `nics` y no se conduce. Lo digo
en el boot y lo vuelvo a decir aquí.

## Instalar de verdad

Solo si quieres borrar un disco.

```text
echos-install
```

El wizard pide confirmación tipo `ERASE-`. Si no la escribes, no formatea.
LIVE no instala nada. `reinstall` es destructivo a propósito.

## Apagar

```text
halt
reboot
```

LIVE es amnésico: lo que no hayas `save` a un disco instalado se evapora.

## Si un comando “no existe”

Hay dos frases distintas, no las mezclo:

- **command not found** — no está en el kernel ni en `epk`.
- **not installed** — el paquete existe en el catálogo; en LIVE a veces
  hay un *stub* (el nombre responde, el compilador de 200 MiB no cabe).

`man <comando>` te saca de dudas. `epk list` también.

## Temas y teclado

```text
termtheme night
termtheme matrix
termtheme ocean
termtheme amber
termtheme paper
kbd es
kbd us
```

Rueda del ratón y PgUp/PgDn hacen scroll. Tab completa nombres de
comando. Flechas recorren `history`.

## Lo que no te voy a vender

No hay navegador en 2.1. `browse` / `nova` son de la línea 1.0 (escritorio)
y no están en Tab. `curl` baja bytes. `curl -dom` espera y escupe texto;
**no ejecuta JavaScript**. Este OS no es para surfear ni para venderse
como escritorio.

— R.N.
