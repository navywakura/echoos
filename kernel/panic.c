/* Panic: print the reason and halt. Serial mirrors it for headless QEMU. */
#include "rogex.h"
#include "arch.h"

ROGEX_NORETURN void panic(const char *msg) {
    console_write("\n[rxos] KERNEL PANIC: ");
    console_write(msg);
    console_write("\n[rxos] halted.\n");
    arch_halt_forever();
}
