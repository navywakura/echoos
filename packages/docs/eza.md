# eza — superior ls

Directory listing with column output, long mode, hidden files and recursion.
Directories always sort before files, names sort alphabetically inside each
group.

## Syntax

```
eza [-laR] [PATH...]
```

## Options

| Option | Effect |
|--------|--------|
| `-l`   | long mode: permissions, link count, size, mtime |
| `-a`   | include hidden entries (`.` and `..` shown too) |
| `-R`   | recurse into subdirectories (depth limit 16) |

Default output flows entries into columns (down-then-across).

## Examples

```
eza
eza -l ~/src
eza -la
eza -R project/
```

## Implementation notes / syscalls

- Per-entry metadata via `stat`; directory reads via
  `opendir/readdir/closedir`.
- Timestamps formatted with `localtime_r + strftime`.
- Own human-size formatter (`B/K/M/G/T`); own `mode_t → rwxrwxrwx` renderer.
