/* Where an unhandled exception ends. Prints what the architecture knows
   before stopping: the syndrome, where it happened and what address it
   touched. A silent reset loop is the alternative, and it teaches nobody. */
#include "arch.h"

static void puts_(const char *s) { while (*s) arch_console_putc(*s++); }

static void puthex(uint64_t v) {
    static const char H[] = "0123456789abcdef";
    int sh;
    puts_("0x");
    for (sh = 60; sh >= 0; sh -= 4) arch_console_putc(H[(v >> sh) & 0xF]);
}

void aarch64_fatal(uint64_t esr, uint64_t elr, uint64_t far) {
    puts_("\n[echOS] fatal exception on aarch64\n  ESR_EL1 ");
    puthex(esr);
    puts_("  class ");
    puthex((esr >> 26) & 0x3F);
    puts_("\n  ELR_EL1 ");
    puthex(elr);
    puts_("  FAR_EL1 ");
    puthex(far);
    puts_("\n  stopped.\n");
    arch_halt_forever();
}
