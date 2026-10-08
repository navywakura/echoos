/* GDT notes — the live table is built in boot.asm. */
#ifndef RXOS_GDT_H
#define RXOS_GDT_H

#define GDT_KERNEL_CODE 0x08   /* boot.asm gdt64.code */
#define GDT_KERNEL_DATA 0x10   /* boot.asm gdt64.data */

#endif /* RXOS_GDT_H */
