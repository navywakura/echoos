# rx-pkg documentation

See `packages/rxpkg/rxpkg.md` for the full usage document and
`packages/SPEC.md` for the `.rxp` container format specification.

Quick reference:

```
rx-pkg build PKGDIR -o OUT.rxp     pack a directory (manifest = metadata)
rx-pkg install FILE.rxp            extract + record (~/.echos/root)
rx-pkg unpack FILE.rxp [-C DIR]    raw extraction, no database update
rx-pkg list                        installed packages
rx-pkg files NAME                  files owned by NAME
rx-pkg info NAME|FILE.rxp          metadata
rx-pkg remove NAME                 uninstall
```
