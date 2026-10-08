# bat — superior cat

Prints files with line numbers, decorative headers between multiple files,
and optional page ranges.

## Syntax

```
bat [-r N:M] [FILE...]
```

## Options

| Option   | Effect |
|----------|--------|
| `-r N:M` | print lines N through M only (1-based). `-r N` prints just line N. |
| `-`      | read stdin |

With multiple files, each gets a `== name ====` header gutter. Without
files, stdin is printed (no header).

## Examples

```
bat main.c
bat -r 10:25 longfile.log
cat a.txt | bat -
bat header.txt body.txt footer.txt
```

## Implementation notes / syscalls

Pure stdio (`fgets/printf`, `fopen` for files). No filesystem calls beyond
open/read/close. Max line length 65536.
