# EchOS custom targets (static ELF)

| Triple | Uso |
|---|---|
| `x86_64-unknown-echos` | kernel y Complete x86_64 |
| `i686-unknown-echos` | x32 / IA-32 |
| `aarch64-unknown-echos` | ARM 64 (Edge boards, Pi-class) |

Rust: `#![no_std]` + `echlibc` o `std` port. Sin GC.

Host:

```
clang --target=x86_64-unknown-none -ffreestanding -static ...
rustc --target x86_64-unknown-none ...
```

`echlibc` syscalls: `sys_open` `sys_read` `sys_write` `sys_mmap` `sys_spawn`.
mmap es una ventana BSS de 64 KiB, no `malloc`.
