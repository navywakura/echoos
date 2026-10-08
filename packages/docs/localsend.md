# localsend — LAN file transfer

Peer-to-peer file transfer over plain TCP, protocol written from scratch.
No mDNS, no HTTP, no external libraries — just POSIX sockets.

## Syntax

```
localsend send FILE HOST [--port P]
localsend receive [--port P] [-o DIR]
```

Default port: **47219**.

## Protocol frame (little-endian throughout)

```
magic     8 bytes   "ECHOSND1"
namelen   u32       length of the file name
name      namelen   UTF-8 bare filename (path components stripped)
size      u64       payload size in bytes
payload   size      raw bytes
```

The receiver validates the magic, sanitizes the name (only a basename is
accepted), streams the payload to disk, and deletes partial files on
interrupted transfers. The receiver loops forever accepting transfers until
killed. `SO_REUSEADDR` is set so it can be restarted immediately.

## Examples

Terminal A:
```
$ localsend receive -o ~/incoming
localsend: listening on 0.0.0.0:47219
received report.pdf (1048576 bytes)
```

Terminal B:
```
$ localsend send report.pdf 192.168.1.42
sent report.pdf (1048576 bytes) to 192.168.1.42:47219
```

## Syscalls / interfaces used

`socket`, `bind`, `listen`, `accept`, `connect`, `send`, `recv`,
`setsockopt`, `close` (sys/socket.h etc. — OS interface only),
`inet_pton`, byte-order conversion done by hand (no htonl for payload
framing). File I/O via stdio.
