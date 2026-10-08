# duf — superior df

Tabular overview of mounted filesystems: device, mount point, type, size,
used, available, use% and an ASCII usage bar.

## Syntax

```
duf [-a]
```

## Options

| Option | Effect |
|--------|--------|
| `-a`   | show all mounts including pseudo filesystems |

By default only real devices (`/dev/*`) and `tmpfs` are listed. Sizes are
binary-scaled and human-formatted (`Ki/Mi/Gi/Ti`). Use% is rounded up at
the halfway point; the bar is 12 cells.

## Example

```
$ duf
FS              MOUNT  TYPE   SIZE    USED   AVAIL  USE%  USAGE
/dev/nvme0n1p3  /      btrfs  474.4Gi 215.7Gi 255.7Gi  45%  [####--------]
```

## Implementation notes / syscalls

- Mount table read directly from `/proc/mounts` with stdio + `sscanf`
  (no getmntent dependency).
- Space figures from `statvfs()` on each mount point.
