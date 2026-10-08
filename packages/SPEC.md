# EchOS `.rxp` Package Format — Specification

**Version:** 1 (RXP1)
**Status:** Stable. Proprietary to Rogex Laboratories / EchOS.
**Scope:** A minimal, from-scratch binary package container used by `epk`
(echOS 2.x; aliases `epk_add` / `epk_delete`) and the legacy `rx-pkg`
binary, and recognized by `unp`. Local media only — no remote repository.

---

## 1. Design goals

1. **No external dependencies.** The format is trivially readable with plain
   C99 + stdio. No zlib, no compression layers, no XML.
2. **Deterministic.** Packing the same directory twice yields byte-identical
   output.
3. **Stream-safe.** All length-prefixed; a reader never needs to seek.

## 2. Container layout

All integers are **little-endian**, packed, no padding.

```
offset  size  field
------  ----  ---------------------------------------------
0       4     magic  = 'R','X','P','1'   (0x31 0x50 0x58 0x52 on disk)
4       4     version (u32), must be 1
8       4     meta_len (u32): byte length of the metadata block
12      4     entry_count (u32)
16      ...   metadata block (meta_len bytes, UTF-8, no NUL terminator)
...     ...   entries, back to back, exactly entry_count of them
EOF
```

### 2.1 Metadata block

Plain text, `Key: value` lines separated by `\n`. Recognized keys:

| Key         | Required | Meaning                          |
|-------------|----------|----------------------------------|
| Name        | yes      | package name `[a-z0-9._-]+`     |
| Version     | yes      | free-form version string        |
| Arch        | no       | e.g. `x86_64`, `any`            |
| Maintainer  | no       | free text                       |
| Description | no       | one-line description            |

Unknown keys MUST be preserved by writers and ignored by readers.

### 2.2 Entry record

```
size   field
-----  -----------------------------------------------------------
2      path_len (u16): byte length of path, 1..4095
...    path (path_len bytes, UTF-8, relative, '/' separators,
       no leading '/', no '..' components, no trailing '/')
4      mode (u32): low 12 bits are the octal permission bits
       (e.g. 0644); type is implied: paths ending in '/' or with
       mode having none of the rwx bits for owner AND recorded via
       a trailing slash are directories — canonical rule:
       DIRECTORIES ARE RECORDED WITH A TRAILING '/' IN path.
8      data_size (u64)
...    payload (data_size bytes, stored uncompressed)
```

Payloads are **always stored raw**. RXP1 deliberately has no compression;
if smaller packages are needed in the future a new magic (`RXPZ`) will be
introduced, keeping RXP1 parsers valid forever.

## 3. Reader rules

1. Verify magic and `version == 1`; refuse anything else.
2. Refuse paths that are absolute or contain `..`.
3. Create parent directories as needed; apply `mode & 0777`.
4. Stop cleanly on truncation with an error naming the offending entry.

## 4. Detection signature

Any file whose first four bytes are `RXP1` is an EchOS package.
`unp` reports it as `rxp archive`; `rx-pkg install` consumes it.

## 5. Reference implementation

- Writer/reader: `packages/rxpkg/rxpkg.c`
- Independent third-party extractor: `packages/coretools/unp.c`
