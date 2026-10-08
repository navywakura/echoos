/* Syscall table stub — unikernel still calls things directly. Reserved for later. */
#include "syscall.h"
#include "arch.h"

void syscall_init(void) {
    klog_info("syscall table: STUB (direct dispatch; no ring-3, no trap gate)");
}

long syscall_dispatch(long n, long a, long b, long c) {
    (void)b; (void)c;
    switch (n) {
    case RX_SYS_LOG:   klog_info((const char *)(uintptr_t)a); return 0;
    case RX_SYS_TICKS: return (long)arch_timer_ticks();
    default:           return -1;
    }
}
