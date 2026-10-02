#ifndef MALEOS_ARCH_GDT_H
#define MALEOS_ARCH_GDT_H

#define KERNEL_CS 0x08
#define KERNEL_DS 0x10
#define TSS_SEL 0x18

void gdt_init(void);

#endif
