# rx-pkg — EchOS package manager

Builds, installs, queries and removes `.rxp` packages (format RXP1, see
`packages/SPEC.md`).

## Syntax

```
rx-pkg build PKGDIR -o OUT.rxp
rx-pkg install FILE.rxp [--root R]
rx-pkg unpack FILE.rxp [-C DIR]
rx-pkg list
rx-pkg files NAME
rx-pkg info NAME|FILE.rxp
rx-pkg remove NAME
```

## Commands

### build

Packs `PKGDIR` into `OUT.rxp`. `PKGDIR/manifest` (plain text) must exist:

```
Name: mytool          # required, [a-z0-9._-]+
Version: 1.2.0        # optional but recommended
Description: does X   # optional
```

Every regular file and directory under PKGDIR is packed **except the
manifest itself**. Output is deterministic: entries are sorted by path and
payloads stored raw.

### install

Extracts the package under the prefix root (`--root`, `$ECHOS_ROOT`, or
default `~/.echos/root`) and writes a database record to
(`$ECHOS_PKGDB` or default) `~/.echos/rxpkg/installed/<name>` containing the
metadata plus every installed path. Refuses unsafe member paths (absolute or
containing `..`).

### unpack

Raw extraction into `-C DIR` (default `.`); no database update.

### list / files / info / remove

Query or undo an installation. `remove` deletes every recorded file and then
best-effort prunes now-empty parent directories up to the root. `info`
inspects an installed record, or a `.rxp` file directly if given a path.

## Exit codes

0 success · 1 error (missing files, bad package, not installed) · 2 usage.

## Environment

| Variable       | Default            | Meaning                    |
|----------------|--------------------|----------------------------|
| `ECHOS_ROOT`   | `~/.echos/root`    | installation prefix        |
| `ECHOS_PKGDB`  | `~/.echos/rxpkg`   | installed-package database |

## Example session

```
$ rx-pkg build ./mypkg -o mytool-1.2.rxp
built mytool-1.2.rxp (5 entries)
$ rx-pkg install mytool-1.2.rxp
installed mytool 1.2 (5 files)
$ rx-pkg files mytool
bin/mytool
share/doc/README
...
$ rx-pkg remove mytool
removed mytool
```

## Syscalls / interfaces used

stdio (`fopen/fread/fwrite/remove`), `mkdir/rmdir/stat/chmod` (sys/stat.h),
`opendir/readdir/closedir` (dirent.h), memory via stdlib. No compression, no
external libraries.
