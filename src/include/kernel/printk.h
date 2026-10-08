#ifndef MALEOS_KERNEL_PRINTK_H
#define MALEOS_KERNEL_PRINTK_H

#include <stdarg.h>
#include <stddef.h>

/*
 * Supported: %c %s %d %i %u %x %X %p %%, optional 0 and - flags, width,
 * length modifiers l, ll and z.
 */
void printk(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void vprintk(const char *fmt, va_list ap);

/* Called before a fatal report so printk no longer waits on its own lock. */
void printk_enter_panic(void);

/* Write raw bytes to the console (serial + VGA) without formatting. */
void printk_write(const char *buf, size_t n);

/* Print a message and halt the machine. */
__attribute__((noreturn, format(printf, 1, 2))) void kpanic(const char *fmt, ...);

#define KASSERT(cond)                                                                              \
    do {                                                                                           \
        if (!(cond))                                                                               \
            kpanic("assertion failed: %s (%s:%d)", #cond, __FILE__, __LINE__);                     \
    } while (0)

#endif
