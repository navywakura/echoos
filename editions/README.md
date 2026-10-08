# echOS editions

Two lines, do not mix them in a pitch.

**2.1 Universal** — one shared Heap-0 CLI kernel. Edition = `epk` manifest
at install time. No desktop. No Echo-in-OS. No native daemons.

```sh
make edition-universal   # build/EchOS-2.1.0-universal-{vm,metal}.iso
make edition-server      # same kernel; rcctl flags only
```

**1.x ECLIPSE** — compile-time flags, desktop (Complete/Minimal) or CLI Edge.

```sh
make edition-complete    # build/EchOS-1.0.0-complete-{vm,metal}.iso
make edition-minimal
make edition-edge
```

| 2.1 edition | What you get |
|---|---|
| Minimal (LIVE) | kernel, ROSH, Heap-0, epk, SNN, Akida probe |
| Complete | same public notes; host toolchain is `epk list --lab` |
| Edge | same + Akida PCI reservation text |
| Server | same + rcctl flags (no httpd/sshd process) |

See `docs2.0/ediciones.md` and `docs2.0/superficie.md`.
