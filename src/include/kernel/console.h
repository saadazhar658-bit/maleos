#ifndef MALEOS_KERNEL_CONSOLE_H
#define MALEOS_KERNEL_CONSOLE_H

#include <stddef.h>

/*
 * The console: keyboard and serial port in, serial port and screen out. Input from both
 * sources is merged into one queue; console_read() applies a minimal line discipline
 * (echo, backspace, Ctrl-C cancels the line, Ctrl-D at the start of a line is end of file).
 */

void console_init(void);  /* serial receive interrupt; call after ioapic_init() */
void console_start(void); /* start the thread that moves keyboard input into the queue */

void console_write(const char *buf, size_t n);
/* Blocks until a line is available. Returns bytes copied, 0 on end of file. */
int console_read(char *buf, size_t n);

/* Queue input as if it had been typed (used by tests). */
void console_feed(const char *s, size_t n);
/* Drop queued input (used by tests). */
void console_flush_input(void);

#endif
