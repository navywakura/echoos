/*
 * Syscall table reserved for a future ring-3 userland.
 * Today apps call through interpreters — no syscall gate yet. Status: STUB.
 */
#ifndef RXOS_SYSCALL_H
#define RXOS_SYSCALL_H

#include "rogex.h"

enum {
    RX_SYS_LOG   = 0,      /* a = (const char*) message                     */
    RX_SYS_TICKS = 1,      /* -> PIT ticks                                  */
    RX_SYS_MAX
};

void syscall_init(void);   /* logs its STUB status */
long syscall_dispatch(long n, long a, long b, long c);

#endif /* RXOS_SYSCALL_H */
