# Comandos

La lista viva está en `CMD_NAMES` (`userland/shell/commands.c`). Tab
lee eso. Aquí, agrupados como yo los pienso. Para flags: `man <cmd>`.

## Orientación

```text
help            lista corta
man / manual    páginas
apropos / man -k
echofetch       tarjeta + logo
about / status / live
```

## Sistema

```text
mem [map]       Heap-0 / PMM; mapa firmware
limits          techos compilados (tareas, SEB, RXFS, WSP…)
robot [run|abi] pipeline sensor -> intención -> safety gate
px4 start|offboard|stop|status   enlace MAVLink 2 con un autopiloto
uptime
date / time     date set YYYY-MM-DD HH:MM:SS
uname / hostname
whoami / id
echo
env [K] [V]     volátil, BSS
power [s]       RAPL si el CPU es Intel y no #GP
reboot / halt / poweroff
clear / cls
sudoers         /etc/sudoers (doas es el binario)
doas <cmd>
```

## Ficheros (nombres BSD y alias Unix)

```text
ls / catalog / dir
cd / wander
pwd / whereami
cat / unveil
write / scribe
rm / vanish / del
cp / forge / copy
mv / relabel / rename
mkdir / homestead
rmdir / raze
tree
head / peep / tail / less
grep / find
nano / pico
history
```

## Disco (destructivos: no en LIVE de paseo)

```text
df / vault / du / dysk
partition / cfdisk / fdisk / gpt / disklabel
format <dev> yes
install / reinstall / echos-install
save / load
```

## Red

```text
www on|off|status|config|dns|get|post|source|channel
curl …            man curl
wdl …             man wdl
wget …
ipconf / ipconfig / ifconfig
nics / lspci-net
dns <host>
tls <host>
ping
traceroute / trace
nmap <host>
wired / mesh / chat / say
```

No hay `browse` / `nova` en 2.1. Eso es el escritorio 1.0. 2.1 no es un
navegador: `curl` / `wdl` bajan bytes.

Ver [red.md](red.md).

## Paquetes y updates

```text
epk list | list --lab | info | install | remove | search | apply
epk_add / epk_delete
rx …              canal legado 1.0 (opcional, www on)
```

`epk list` son notas del kernel. `epk list --lab` son toolchain/daemons
de **host**: no corren aquí. Lista viva: [superficie.md](superficie.md).

## Neuromórfico

```text
hwprobe / lspci
bench / bench-snn
prisma5 [alpha|null|stress]    # drive sintético del cubo Q6, no EEG
neuro / neurocpu [software|akida]
```

No hay chatbot ni LLM en este unikernel. `loihi` tampoco.

## OpenBSD-ish

```text
rcctl enable|disable|status|start|stop   # flags; no hay daemons
pfctl -e|-d|-f|-s|-sr                    # boolean RAM; no filtra
doas <cmd>                               # un espacio de direcciones
top / rxmon
```

## Split / apps de escritorio

No. Una consola. `run` / `apps` / paneles son línea 1.0.

## Convenciones que me importan

- **unknown vs not installed:** dos frases. La primera es un typo. La
  segunda es `epk`.
- **No hay `sudo`.** `doas`.
- **No hay `apt`.** `epk`.
- **No hay `ifconfig` de Linux.** `ipconf` (el alias existe porque
  todo el mundo lo escribe).
- Un comando que formatea pide `yes` o `ERASE-`. Si no, no hace nada.

Si añades un comando: `CMD_NAMES`, dispatcher, `man` page. Los tres.
Si omites el man, el usuario tiene que leer el C. Eso es fallar.

— R.N.
