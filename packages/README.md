# EchOS Packages

The package subsystem of **EchOS** (Rogex Laboratories). 100% proprietary,
written from scratch in portable C99. No external libraries of any kind are
used — only libc stdio/stdlib/string/ctype, dirent, sys/stat and (where an OS
interface is unavoidable) POSIX sockets/statvfs/unistd.

## Contents

```
packages/
├── README.md            this file
├── SPEC.md              .rxp container format specification
├── Makefile             builds everything into bin/
├── rxpkg/
│   ├── rxpkg.c          rx-pkg / epk: the EchOS package manager (RXP1)
│   └── rxpkg.md         usage documentation
├── epk/
│   └── epk.md           echOS 2.x command names (epk_add / epk_delete)
├── coretools/           one self-contained .c per tool
│   ├── rg.c             superior grep  (recursive, -i -v -n)
│   ├── fd.c             superior find   (glob * ?, -t f|d, -H hidden, -e ext)
│   ├── bat.c            superior cat    (numbers, headers, -r first:last)
│   ├── eza.c            superior ls     (-l long, -a hidden, -R recurse)
│   ├── z.c              zoxide-like cd  (~/.echos/z.db frecency database)
│   ├── unp.c            intelligent unpacker (magic-byte detection; tar,
│   │                    stored zip, .rxp extractors written from scratch)
│   ├── dust.c           superior du     (largest-first tree + % bars, -d depth)
│   ├── duf.c            superior df     (/proc/mounts + statvfs, ASCII bars)
│   ├── hyperfine.c      benchmarking    (--runs N, CLOCK_MONOTONIC stats)
│   └── localsend.c      LAN file transfer over raw TCP (port 47219)
├── docs/                per-tool documentation (syntax, options, syscalls)
└── tests/run_tests.sh   smoke-test suite (`make test`)
```

## Build

```
cd packages
make            # cc -std=c99 -O2 -Wall -Wextra, zero warnings
make test       # runs the full smoke suite
```

## Tool index

| Tool | One-liner | Docs |
|------|-----------|------|
| `rg` | recursive text search with line numbers | docs/rg.md |
| `fd` | recursive filename glob finder | docs/fd.md |
| `bat` | cat with numbers, headers and ranges | docs/bat.md |
| `eza` | column / long / recursive directory listing | docs/eza.md |
| `z` | frecency-based directory jumper | docs/z.md |
| `unp` | detect-by-magic unpacker (tar/zip/rxp) | docs/unp.md |
| `dust` | disk usage tree with percentage bars | docs/dust.md |
| `duf` | mounted filesystem overview table | docs/duf.md |
| `hyperfine` | command benchmarking with statistics | docs/hyperfine.md |
| `localsend` | LAN file transfer, framed TCP protocol | docs/localsend.md |
| `rx-pkg` | build/install/remove .rxp packages | docs/rx-pkg.md, rxpkg/rxpkg.md |

## Design rules

1. Every tool is a single self-contained `.c` file.
2. Everything is hand-rolled: glob matcher, human sizes, tar reader, zip
   reader, sqrt, exponential decay, framed network protocol.
3. No compression is ever performed (no zlib); `.rxp` stores payloads raw.
4. All tools compile warning-free with `cc -std=c99 -O2 -Wall -Wextra`.
