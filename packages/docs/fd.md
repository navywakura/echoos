# fd — superior find

Recursive filename search using an own glob matcher (`*` and `?`).

## Syntax

```
fd [options] [PATTERN] [PATH]
```

Default pattern matches everything; default path is `.`.

## Options

| Option   | Effect |
|----------|--------|
| `-t f`   | files only |
| `-t d`   | directories only |
| `-H`     | include hidden entries |
| `-e EXT` | match extension `EXT` exactly (e.g. `-e c`) |

The pattern is matched against the basename. A lone `.` pattern is accepted
as find-style "match everything". Exit status: 0 if anything was found.

## Examples

```
fd '*.c' src/
fd -t d node_modules
fd -e md -H . ~/notes
fd 'test_?.py' .
```

## Implementation notes / syscalls

- Own iterative two-pointer wildcard matcher (backtracking only on `*`),
  no regex.
- Traversal via `opendir/readdir/closedir`, type checks via `stat`;
  depth limit 128 to survive cycles.
