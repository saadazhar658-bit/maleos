#include "kernel/printk.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "arch/cpu.h"
#include "arch/serial.h"
#include "arch/vga.h"
#include "kernel/spinlock.h"

static spinlock_t print_lock = SPINLOCK_INIT;
static volatile bool panicking;

static void console_putc(char c)
{
    if (c == '\n')
        serial_putc('\r');
    serial_putc(c);
    vga_putc(c);
}

static void put_padded(const char *s, size_t len, int width, char pad, bool left)
{
    if (!left) {
        for (size_t i = len; i < (size_t)width; i++)
            console_putc(pad);
    }
    for (size_t i = 0; i < len; i++)
        console_putc(s[i]);
    if (left) {
        for (size_t i = len; i < (size_t)width; i++)
            console_putc(' ');
    }
}

static void put_number(unsigned long long v, unsigned base, bool upper, bool negative, int width,
                       bool zero, bool left)
{
    const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    char tmp[24];
    size_t n = 0;

    if (v == 0)
        tmp[n++] = '0';
    while (v) {
        tmp[n++] = digits[v % base];
        v /= base;
    }

    char out[26];
    size_t len = 0;
    if (negative)
        out[len++] = '-';
    while (n)
        out[len++] = tmp[--n];

    if (zero && negative && !left) {
        /* Sign first, then zero padding. */
        console_putc('-');
        put_padded(out + 1, len - 1, width - 1, '0', false);
    } else {
        put_padded(out, len, width, zero && !left ? '0' : ' ', left);
    }
}

static void vprintk_unlocked(const char *fmt, va_list ap)
{
    for (; *fmt; fmt++) {
        if (*fmt != '%') {
            console_putc(*fmt);
            continue;
        }
        fmt++;

        bool zero = false;
        bool left = false;
        int width = 0;
        int lng = 0; /* 0 = int, 1 = long, 2 = long long */

        for (;; fmt++) {
            if (*fmt == '0')
                zero = true;
            else if (*fmt == '-')
                left = true;
            else
                break;
        }
        while (*fmt >= '0' && *fmt <= '9')
            width = width * 10 + (*fmt++ - '0');
        while (*fmt == 'l' || *fmt == 'z') {
            lng++;
            fmt++;
        }

        switch (*fmt) {
        case 'c':
            console_putc((char)va_arg(ap, int));
            break;
        case 's': {
            const char *s = va_arg(ap, const char *);
            if (!s)
                s = "(null)";
            size_t len = 0;
            while (s[len])
                len++;
            put_padded(s, len, width, ' ', left);
            break;
        }
        case 'd':
        case 'i': {
            long long v = lng ? va_arg(ap, long long) : va_arg(ap, int);
            bool neg = v < 0;
            put_number(neg ? 0ULL - (unsigned long long)v : (unsigned long long)v, 10, false, neg,
                       width, zero, left);
            break;
        }
        case 'u':
        case 'x':
        case 'X': {
            unsigned long long v = lng ? va_arg(ap, unsigned long long) : va_arg(ap, unsigned int);
            put_number(v, *fmt == 'u' ? 10 : 16, *fmt == 'X', false, width, zero, left);
            break;
        }
        case 'p':
            console_putc('0');
            console_putc('x');
            put_number((uintptr_t)va_arg(ap, void *), 16, false, false, 16, true, false);
            break;
        case '%':
            console_putc('%');
            break;
        case '\0':
            return;
        default:
            console_putc('%');
            console_putc(*fmt);
            break;
        }
    }
}

void printk_enter_panic(void)
{
    panicking = true;
}

void vprintk(const char *fmt, va_list ap)
{
    uint64_t flags = 0;
    if (!panicking)
        flags = spin_lock_irqsave(&print_lock);
    vprintk_unlocked(fmt, ap);
    if (!panicking)
        spin_unlock_irqrestore(&print_lock, flags);
}

void printk(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vprintk(fmt, ap);
    va_end(ap);
}

void kpanic(const char *fmt, ...)
{
    va_list ap;
    printk_enter_panic();
    printk("\n*** KERNEL PANIC: ");
    va_start(ap, fmt);
    vprintk(fmt, ap);
    va_end(ap);
    printk(" ***\n");
    halt_forever();
}
