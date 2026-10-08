# z — zoxide-like directory jumper

Maintains a frecency (frequency × recency) database of visited directories
and answers "best directory for this term" queries. Pair it with a shell
function to get smart `cd`.

## Syntax

```
z add PATH        record a visit (path canonicalized)
z query TERM      print the best-ranked dir whose path contains TERM (case-insensitive)
z query           same as above with empty term (most recent overall)
z list            dump the whole table ranked by score
```

## Ranking

```
score = frequency * e^(-age_days / half_life)
half_life = 14 days
```

The exponential is computed by our own Taylor series (`e^-x`), no libm.
Database lives at `~/.echos/z.db`, one `freq<TAB>timestamp<TAB>path` record
per line; the directory is created on first write.

## Shell integration

```sh
z() { local d; d=$(~/.echos/bin/z query "${1:-}"); [ -n "$d" ] && cd "$d"; }
# and in PROMPT_COMMAND / precmd:  z add "$PWD"
```

## Examples

```
$ z add /usr/share/doc
$ z add /usr/lib
$ z query doc
/usr/share/doc
```

## Implementation notes / syscalls

- Database I/O via stdio (`fgets/fprintf`), `strdup/realloc` tables,
  `qsort` ranking, `time(NULL)` for recency, `realpath` to canonicalize.
- No external libraries.
