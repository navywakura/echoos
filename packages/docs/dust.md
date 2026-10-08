# dust — superior du

Recursive disk usage printed as a largest-first tree with ASCII percentage
bars relative to the scanned root.

## Syntax

```
dust [-d DEPTH] [PATH]
```

## Options

| Option    | Effect |
|-----------|--------|
| `-d DEPTH`| maximum tree depth shown (default 3) |
| `PATH`    | what to scan (default `.`) |

The root line always shows the grand total at 100%. Subdirectories are
listed largest-first; children holding ≥10% of the total are expanded
recursively until the depth limit. Sizes are apparent sizes (`st_size`),
human-formatted (`B/K/M/G/T/P`).

## Example

```
$ dust -d 1 ~/build
61.5 G   [####################] 100%  build
22.0 G   [#######-------------]  34%  obj
18.3 G   [#####---------------]  29%  images
...
```

## Implementation notes / syscalls

- Recursive size accumulation over `opendir/readdir/closedir`; per-entry
  sizes from `stat`. Sorting with `qsort`.
- Own human-size formatter; no libm anywhere.
