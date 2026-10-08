# epk — echOS Package

Local-only package manager for echOS 2.x Universal. Replaces the `pkg` /
`rx-pkg` names on the 2.0 CLI. There is **no remote repository**: `epk`
reads `.rxp` files and edition manifests from the install medium
(`/media/epk` on the ISO, `$EPK_MEDIA` on a host).

The on-disk format is still **RXP1** (`packages/SPEC.md`). `epk` is the
command; the container did not change.

## Commands

| Command | Alias | Action |
|---|---|---|
| `epk install FILE.rxp` | `epk add`, `epk_add` | extract + record |
| `epk remove NAME` | `epk delete`, `epk_delete` | delete files + record |
| `epk list` | | installed packages |
| `epk info [NAME\|FILE.rxp]` | | metadata (no arg = media status in-kernel) |
| `epk files NAME` | | list owned files (host) |
| `epk unpack FILE.rxp -C DIR` | | extract without db |
| `epk apply EDITION` | | in-kernel: apply `minimal.manifest` … `server.manifest` |

## Editions (shared kernel)

The ISO ships one Heap-0 kernel ELF. `epk apply` selects the package set:

- `minimal.manifest`
- `complete.manifest`
- `edge.manifest`
- `server.manifest`

## Build

```
cd packages && make
# binaries: bin/epk  bin/epk_add  bin/epk_delete  bin/rx-pkg
```
