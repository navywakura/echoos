# rg — superior grep

Recursive text search for EchOS. Prints `path:line:content` per hit.

## Syntax

```
rg [options] PATTERN [PATH...]
```

`PATH` may be a file or a directory (recursed). Default path is `.`
(recursive). Use `-` to read stdin.

## Options

| Option | Effect |
|--------|--------|
| `-i`   | ignore case |
| `-v`   | invert match — print non-matching lines |
| `-n`   | prefix line numbers (default on) |
| `-N`   | suppress line numbers |

Hidden files/directories (leading `.`) are skipped. Exit status: 0 if any
match, 1 otherwise, 2 on usage error. A summary goes to stderr.

## Examples

```
echo hello | rg hello -
rg -i TODO src/
rg -v '^$' notes.md
```

## Implementation notes / syscalls

- Plain substring matching with an own case-insensitive scanner (`ci_strstr`);
  no regex engine.
- Directory traversal via `opendir/readdir/closedir`; file type checks via
  `stat`. File I/O via stdio (`fopen/fgets`). Max line length 65536.
