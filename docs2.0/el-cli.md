# El CLI

La interfaz de echOS 2.1 *es* esto. No hay un “modo texto” de un
escritorio. No hay `startx`.

## Contrato visual

- Tipografía: **Liberation Mono**, célula 8×16 en framebuffer.
- Serial: ANSI, 80 columnas de cortesía (el TUI es 80×25).
- Paleta del CLI, no del chrome 1.0:

```text
termtheme          # rota
termtheme night    # por defecto, oscuro
termtheme matrix
termtheme ocean
termtheme amber
termtheme paper    # claro; por si acaso
```

`termcolor` es alias. Aplica ya. `clear` / `cls` usan el fondo de esa
paleta, no un verde fosforescente que se me coló en 1.0.

## Prompt

```text
usuario@echos /ruta/cwd#
```

Colores: usuario / host / path. Caret parpadeante. En FB el cursor es
software; en serial, el de la TTY.

## Edición de línea

| Tecla | Qué |
|---|---|
| Tab | completa el comando (lista `CMD_NAMES`) |
| Arriba / Abajo | historial |
| PgUp / PgDn | scrollback (~120 líneas) |
| Rueda | igual, 0x08 / 0x10 del paquete PS/2 |
| Ctrl+C | en algunos sitios; no es POSIX |

`history` imprime el anillo. No se guarda a disco (amnesia).

## TUI del instalador

`userland/cli/tui.c`. Un frame compacto por serial (no un CSI por
celda: eso inundaba COM1). Flechas, Enter, Esc, `q`, números 1–9 para
saltar. Ratón oculto en CLI.

El editor `nano` es otro TUI: números de línea, syntax C/Python/JS/
HTML/Rust. Ctrl+W guarda. Esc sale. `pico` es el mismo binario.

## Logo y banner

- **Banner de boot:** FIGlet `echOS`.
- **echofetch:** logo Braille de uso libre a la izquierda, specs a la derecha.

## `man`

Es el comando más útil que he puesto en esta máquina. `help` es una
lista. `man curl` es una página: NAME, SYNOPSIS, OPTIONS, EXAMPLES.

```text
man
man curl
man 1 wdl          # el número de sección se ignora; no soy man-db
manual ipconf      # alias
man -k tls
apropos download
```

Páginas en `userland/cli/man.c`. Si un comando existe y no tiene
página, `man` te manda a `help`. Eso es un bug mío, no tuyo: ábreme
issue o métela.

## Idioma

El wizard es ES/EN. Los comandos son inglés (unix/BSD). Las páginas
`man` de red las escribí en inglés de laboratorio mezclado; `help`
está mezclado ES/EN porque así salió el árbol. No voy a fingir una
i18n de gettext.

Teclado: `kbd es` / `kbd us` / `kbd mac`. Acentos y ñ en ES.

## Scrollback

Anillo de 120 filas. No es `less` del mundo GNU. `less` existe como
comando de fichero. El scroll de consola es global.

## Lo que el CLI no hace

- Multiplexor real tipo tmux (el comando `tmux` en LIVE es stub).
- Unicode completo. Latin-1 + Braille del logo + UTF-8 que no rompa.
- Copiar/pegar X11. Serial y teclado.

— R.N.
