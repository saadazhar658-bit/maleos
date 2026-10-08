#include "ulib.h"

struct out {
    int fd;
    char buf[256];
    size_t len;
    int total;
};

static void flush(struct out *o)
{
    size_t done = 0;
    while (done < o->len) {
        long n = write(o->fd, o->buf + done, o->len - done);
        if (n <= 0)
            break;
        done += (size_t)n;
    }
    o->len = 0;
}

static void put(struct out *o, char c)
{
    if (o->len == sizeof(o->buf))
        flush(o);
    o->buf[o->len++] = c;
    o->total++;
}

static void pad(struct out *o, int n, char c)
{
    while (n-- > 0)
        put(o, c);
}

static void emit(struct out *o, const char *s, size_t len, int width, char fill, int left)
{
    if (!left)
        pad(o, width - (int)len, fill);
    for (size_t i = 0; i < len; i++)
        put(o, s[i]);
    if (left)
        pad(o, width - (int)len, ' ');
}

int vdprintf(int fd, const char *fmt, va_list ap)
{
    struct out o = {.fd = fd};
    for (; *fmt; fmt++) {
        if (*fmt != '%') {
            put(&o, *fmt);
            continue;
        }
        fmt++;
        int left = 0, zero = 0, width = 0, longs = 0;
        for (;; fmt++) {
            if (*fmt == '-')
                left = 1;
            else if (*fmt == '0')
                zero = 1;
            else
                break;
        }
        while (*fmt >= '0' && *fmt <= '9')
            width = width * 10 + (*fmt++ - '0');
        while (*fmt == 'l' || *fmt == 'z') {
            longs++;
            fmt++;
        }

        char tmp[32];
        switch (*fmt) {
        case 'c':
            put(&o, (char)va_arg(ap, int));
            break;
        case 's': {
            const char *s = va_arg(ap, const char *);
            if (!s)
                s = "(null)";
            emit(&o, s, strlen(s), width, ' ', left);
            break;
        }
        case 'd':
        case 'i':
        case 'u':
        case 'o':
        case 'x':
        case 'X':
        case 'p': {
            int is_signed = *fmt == 'd' || *fmt == 'i';
            unsigned base = (*fmt == 'u' || is_signed) ? 10 : *fmt == 'o' ? 8 : 16;
            const char *digits = *fmt == 'X' ? "0123456789ABCDEF" : "0123456789abcdef";
            uint64_t v;
            int neg = 0;
            if (*fmt == 'p') {
                v = (uint64_t)va_arg(ap, void *);
            } else if (is_signed) {
                int64_t s = longs ? va_arg(ap, long) : va_arg(ap, int);
                neg = s < 0;
                v = neg ? (uint64_t)0 - (uint64_t)s : (uint64_t)s;
            } else {
                v = longs ? va_arg(ap, unsigned long) : va_arg(ap, unsigned);
            }
            size_t n = 0;
            do {
                tmp[n++] = digits[v % base];
                v /= base;
            } while (v);
            char num[40];
            size_t m = 0;
            if (neg)
                num[m++] = '-';
            if (*fmt == 'p') {
                num[m++] = '0';
                num[m++] = 'x';
            }
            while (n)
                num[m++] = tmp[--n];
            if (zero && !left && width > (int)m) {
                size_t prefix = neg ? 1 : (*fmt == 'p' ? 2 : 0);
                for (size_t i = 0; i < prefix; i++)
                    put(&o, num[i]);
                pad(&o, width - (int)m, '0');
                for (size_t i = prefix; i < m; i++)
                    put(&o, num[i]);
            } else {
                emit(&o, num, m, width, ' ', left);
            }
            break;
        }
        case '%':
            put(&o, '%');
            break;
        default:
            put(&o, '%');
            if (*fmt)
                put(&o, *fmt);
            else
                fmt--;
            break;
        }
    }
    flush(&o);
    return o.total;
}

int dprintf(int fd, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vdprintf(fd, fmt, ap);
    va_end(ap);
    return n;
}

int printf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vdprintf(1, fmt, ap);
    va_end(ap);
    return n;
}

int puts(const char *s)
{
    return dprintf(1, "%s\n", s);
}

int readline(char *buf, size_t max)
{
    long n = read(0, buf, max - 1);
    if (n <= 0)
        return -1;
    if (buf[n - 1] == '\n')
        n--;
    buf[n] = 0;
    return (int)n;
}
