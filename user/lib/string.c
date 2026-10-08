#include "ulib.h"

size_t strlen(const char *s)
{
    size_t n = 0;
    while (s[n])
        n++;
    return n;
}

int strcmp(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return (unsigned char)*a - (unsigned char)*b;
}

int strncmp(const char *a, const char *b, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        if (a[i] != b[i])
            return (unsigned char)a[i] - (unsigned char)b[i];
        if (!a[i])
            return 0;
    }
    return 0;
}

size_t strlcpy(char *dst, const char *src, size_t size)
{
    size_t n = strlen(src);
    if (size) {
        size_t c = n < size - 1 ? n : size - 1;
        memcpy(dst, src, c);
        dst[c] = 0;
    }
    return n;
}

char *strchr(const char *s, int c)
{
    for (;; s++) {
        if (*s == (char)c)
            return (char *)s;
        if (!*s)
            return NULL;
    }
}

void *memcpy(void *dst, const void *src, size_t n)
{
    uint8_t *d = dst;
    const uint8_t *s = src;
    while (n--)
        *d++ = *s++;
    return dst;
}

void *memmove(void *dst, const void *src, size_t n)
{
    uint8_t *d = dst;
    const uint8_t *s = src;
    if (d < s) {
        while (n--)
            *d++ = *s++;
    } else {
        d += n;
        s += n;
        while (n--)
            *--d = *--s;
    }
    return dst;
}

void *memset(void *dst, int c, size_t n)
{
    uint8_t *d = dst;
    while (n--)
        *d++ = (uint8_t)c;
    return dst;
}

int memcmp(const void *a, const void *b, size_t n)
{
    const uint8_t *x = a, *y = b;
    for (size_t i = 0; i < n; i++) {
        if (x[i] != y[i])
            return x[i] - y[i];
    }
    return 0;
}

long atol(const char *s)
{
    long v = 0;
    int neg = 0;
    if (*s == '-') {
        neg = 1;
        s++;
    }
    while (*s >= '0' && *s <= '9')
        v = v * 10 + (*s++ - '0');
    return neg ? -v : v;
}

const char *strerror(int err)
{
    switch (err) {
    case EPERM:
        return "operation not permitted";
    case ENOENT:
        return "no such file or directory";
    case ESRCH:
        return "no such process";
    case EIO:
        return "I/O error";
    case E2BIG:
        return "argument list too long";
    case ENOEXEC:
        return "not an executable";
    case EBADF:
        return "bad file descriptor";
    case ECHILD:
        return "not a child of this process";
    case ENOMEM:
        return "out of memory";
    case EACCES:
        return "permission denied";
    case EFAULT:
        return "bad address";
    case EBUSY:
        return "busy";
    case EEXIST:
        return "file exists";
    case ENOTDIR:
        return "not a directory";
    case EISDIR:
        return "is a directory";
    case EINVAL:
        return "invalid argument";
    case EMFILE:
        return "too many open files";
    case EFBIG:
        return "file too large";
    case ENOSPC:
        return "no space left";
    case EROFS:
        return "read-only file system";
    case ERANGE:
        return "result too large";
    case ENAMETOOLONG:
        return "name too long";
    case ENOSYS:
        return "function not implemented";
    case ENOTEMPTY:
        return "directory not empty";
    case ELOOP:
        return "too many levels of symbolic links";
    case ENODEV:
        return "no such device";
    default:
        return "error";
    }
}
