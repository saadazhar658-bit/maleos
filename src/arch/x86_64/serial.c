#include "arch/serial.h"

#include "arch/cpu.h"

#define COM1 0x3F8

void serial_init(void)
{
    outb(COM1 + 1, 0x00); /* disable interrupts */
    outb(COM1 + 3, 0x80); /* enable DLAB */
    outb(COM1 + 0, 0x03); /* divisor 3 -> 38400 baud */
    outb(COM1 + 1, 0x00);
    outb(COM1 + 3, 0x03); /* 8 bits, no parity, one stop bit */
    outb(COM1 + 2, 0xC7); /* enable and clear FIFO */
    outb(COM1 + 4, 0x0B); /* DTR, RTS, OUT2 */
}

void serial_putc(char c)
{
    /* Bounded wait so a machine without a serial port does not hang. */
    for (int spin = 0; spin < 100000 && !(inb(COM1 + 5) & 0x20); spin++)
        ;
    outb(COM1, (uint8_t)c);
}
