# unp — intelligent unpacker

Detects archive formats by **magic bytes** (never by file extension) and
either reports the action it would take or performs it with `-x`.

## Syntax

```
unp [-x] [-C DIR] FILE
```

## Options

| Option  | Effect |
|---------|--------|
| `-x`    | perform the extraction (default: dry-run report) |
| `-C DIR`| extract into DIR (created if needed; default: current dir) |

## Recognized formats

| Magic | Format | Action |
|-------|--------|--------|
| `ustar` at offset 257 (or valid tar checksum) | tar | extract — own reader supporting ustar prefix fields and GNU longname (`L`) entries |
| `1f 8b`                                       | gzip | detection only — decompression is intentionally out of scope (no compression code anywhere in EchOS packages) |
| `PK\x03\x04`                                  | zip | extract stored/method-0 entries; deflated entries are reported and skipped |
| `RXP1`                                        | EchOS package | unpack per SPEC.md |

Extraction refuses unsafe member names (absolute paths, `..` components).

## Examples

```
unp backup.tar              # prints "tar archive ... (run with -x to extract)"
unp -x backup.tar
unp -x -C /tmp/out app.rxp
```

## Implementation notes / syscalls

- Binary I/O via stdio (`fread/fwrite/rewind/fseek`).
- Extraction creates parent directories with `mkdir`; output files via
  `fopen`. No external libraries, no zlib, no libtar.
- Tar reader handles: regular files (`0`/`\0`/`7`), directories (`5`),
  block padding, GNU long names, and skips specials/links safely.
