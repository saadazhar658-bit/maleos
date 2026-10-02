#ifndef MALEOS_ARCH_VGA_H
#define MALEOS_ARCH_VGA_H

#include <stdint.h>

void vga_init(void);
void vga_putc(char c);
/* Re-point the text buffer, e.g. after the identity map is gone. */
void vga_set_base(uint16_t *base);

#endif
