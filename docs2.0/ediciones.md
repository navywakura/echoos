# Ediciones

Un kernel. Cuatro manifiestos. LIVE es Minimal en RAM.

## 1.0 vs 2.1 (otra vez, porque se confunde)

En **1.0** Complete / Minimal / Edge son *flags de compilación*.
Tres ISOs, tres ELFs. Minimal no es Complete capado a medias: el
desktop es el mismo, ECHO e IDE salen del compile.

En **2.1 Universal** el ELF es uno. La edición se aplica con
`epk apply` en el wizard o a mano. El runtime queda en
`/etc/echos-edition`.

Server 2.1 también es Heap-0 CLI. No hay daemons nativos. No es 1.0.

## Las cuatro

### Minimal

Kernel, ROSH, Heap-0, `epk`, `echofetch`, `doas` / `rcctl` / `pfctl`.
Es el LIVE. Es lo que arranca si pulsas `q`.

Para: ver el OS, instalar después, máquina pequeña.

### Complete (texto)

Las mismas notas públicas que Minimal más SNN/Akida. Toolchain
(tcc, python, rustc…) **no** entra en `epk apply complete`. Vive en
`epk list --lab`. Compiladores de verdad: host + `echlibc`.

Para: el mismo unikernel con las notas de laboratorio a la vista.

### Edge

Minimal más runtime SNN, I/O estático, sonda Akida AKD1000,
`bench-snn` / `prisma5`. Sin daemons. Sin WM (2.1 no tiene WM
en ninguna edición). Sin offload hasta que haya placa.

Para: robot, cámara, placa con NPU. Presupuesto de memoria
honesto: LIVE `gfxpayload=text` si 16 MiB importan. El
backbuffer 1280×720×32 son ~3.6 MiB.

### Server

Minimal más la misma pila IPv4. flags `httpd` / `ftpd` / `sshd` /
`pqc` en `rcctl`. `rcctl enable httpd` **anota**. No hay nginx.
ROGEX-PQC = ML-KEM en `rogex-core`, no un proceso túnel.

Para: el día que los daemons existan. Hoy: el manifiesto y el
cliente TCP.

## Manifiestos

```text
editions/minimal.manifest
editions/complete.manifest
editions/edge.manifest
editions/server.manifest
```

Texto plano, una lista. `epk apply complete` los recorre.

## Cómo se elige

Wizard paso 5, o:

```text
epk apply edge
```

Build de ISO:

```sh
make edition-universal    # 2.1 CLI, la que documenta este docs2.0/
make edition-server
make edition-complete     # 1.0 desktop
make edition-minimal      # 1.0 desktop sin ECHO
make edition-edge         # 1.x CLI-only
```

Si pides “la ISO de Edge” a secas, pregunta: ¿1.x o 2.0 apply?
No son el mismo binario.

— R.N.
