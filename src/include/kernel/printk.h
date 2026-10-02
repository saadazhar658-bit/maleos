#ifndef MALEOS_KERNEL_PRINTK_H
#define MALEOS_KERNEL_PRINTK_H

#include <stdarg.h>

/*
 * Supported: %c %s %d %i %u %x %X %p %%, optional 0 flag and width,
 * length modifiers l, ll and z.
 */
void printk(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void vprintk(const char *fmt, va_list ap);

/* Print a message and halt the machine. */
__attribute__((noreturn, format(printf, 1, 2))) void kpanic(const char *fmt, ...);

#define KASSERT(cond)                                                                              \
    do {                                                                                           \
        if (!(cond))                                                                               \
            kpanic("assertion failed: %s (%s:%d)", #cond, __FILE__, __LINE__);                     \
    } while (0)

#endif
