# hyperfine — command benchmarking

Runs a shell command repeatedly, times each execution with
`clock_gettime(CLOCK_MONOTONIC)`, and reports distribution statistics.

## Syntax

```
hyperfine [--runs N] 'COMMAND'
```

## Options

| Option     | Effect |
|------------|--------|
| `--runs N` | number of runs (default 10, max 100000) |

The command is executed through `system()`, so shell syntax (pipes,
redirections, quoting) works. Reports:

- **min / mean / max** run time in milliseconds
- **stddev** in ms and as a percentage of the mean (sample stddev, n−1)
- fastest/slowest summary with the best-case speedup factor

Square roots for stddev are computed by our own Newton's-method `sqrt` —
no libm.

## Examples

```
hyperfine --runs 3 true
hyperfine --runs 50 './mytool index.dat > /dev/null'
hyperfine 'grep -r needle .'
```

## Implementation notes / syscalls

`clock_gettime` (POSIX timers), `system` (fork+exec via libc), stdio for
output.
