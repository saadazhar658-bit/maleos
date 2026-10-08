#ifndef MALEOS_ULIB_H
#define MALEOS_ULIB_H

/*
 * A tiny C library for Maleos user programs: system call wrappers, string helpers and
 * printf. Wrappers return the kernel's result unchanged: >= 0 on success, -errno on failure
 * (there is no errno variable).
 */

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

#include "abi/abi.h"
#include "kernel/errno.h"

static inline long syscall3(long nr, long a, long b, long c)
{
    long ret;
    __asm__ volatile("syscall"
                     : "=a"(ret)
                     : "a"(nr), "D"(a), "S"(b), "d"(c)
                     : "rcx", "r11", "memory");
    return ret;
}

__attribute__((noreturn)) static inline void exit(int code)
{
    syscall3(SYS_EXIT, code, 0, 0);
    for (;;)
        ;
}
static inline long write(int fd, const void *buf, size_t n)
{
    return syscall3(SYS_WRITE, fd, (long)buf, (long)n);
}
static inline long read(int fd, void *buf, size_t n)
{
    return syscall3(SYS_READ, fd, (long)buf, (long)n);
}
static inline long open(const char *path, int flags)
{
    return syscall3(SYS_OPEN, (long)path, flags, 0);
}
static inline long close(int fd)
{
    return syscall3(SYS_CLOSE, fd, 0, 0);
}
static inline long lseek(int fd, long off, int whence)
{
    return syscall3(SYS_LSEEK, fd, off, whence);
}
static inline long stat(const char *path, struct stat *st)
{
    return syscall3(SYS_STAT, (long)path, (long)st, 0);
}
static inline long lstat(const char *path, struct stat *st)
{
    return syscall3(SYS_LSTAT, (long)path, (long)st, 0);
}
static inline long fstat(int fd, struct stat *st)
{
    return syscall3(SYS_FSTAT, fd, (long)st, 0);
}
static inline long readdir(int fd, struct dirent *de)
{
    return syscall3(SYS_READDIR, fd, (long)de, 0);
}
static inline long mkdir(const char *path)
{
    return syscall3(SYS_MKDIR, (long)path, 0, 0);
}
static inline long rmdir(const char *path)
{
    return syscall3(SYS_RMDIR, (long)path, 0, 0);
}
static inline long unlink(const char *path)
{
    return syscall3(SYS_UNLINK, (long)path, 0, 0);
}
static inline long symlink(const char *target, const char *path)
{
    return syscall3(SYS_SYMLINK, (long)target, (long)path, 0);
}
static inline long readlink(const char *path, char *buf, size_t size)
{
    return syscall3(SYS_READLINK, (long)path, (long)buf, (long)size);
}
static inline long chdir(const char *path)
{
    return syscall3(SYS_CHDIR, (long)path, 0, 0);
}
static inline long getcwd(char *buf, size_t size)
{
    return syscall3(SYS_GETCWD, (long)buf, (long)size, 0);
}
static inline long getpid(void)
{
    return syscall3(SYS_GETPID, 0, 0, 0);
}
static inline void yield(void)
{
    syscall3(SYS_YIELD, 0, 0, 0);
}
static inline void sleep_ms(unsigned long ms)
{
    syscall3(SYS_SLEEP_MS, (long)ms, 0, 0);
}
static inline unsigned long uptime_ms(void)
{
    return (unsigned long)syscall3(SYS_UPTIME_MS, 0, 0, 0);
}
static inline long spawn(const char *path, char *const *argv, const struct abi_spawn_attr *attr)
{
    return syscall3(SYS_SPAWN, (long)path, (long)argv, (long)attr);
}
static inline long wait(int pid, int *status)
{
    return syscall3(SYS_WAIT, pid, (long)status, 0);
}
static inline long sbrk(long increment)
{
    return syscall3(SYS_SBRK, increment, 0, 0);
}
static inline long procinfo(struct abi_procinfo *buf, unsigned max)
{
    return syscall3(SYS_PROCINFO, (long)buf, max, 0);
}
static inline long meminfo(struct abi_meminfo *mi)
{
    return syscall3(SYS_MEMINFO, (long)mi, 0, 0);
}
__attribute__((noreturn)) static inline void poweroff(void)
{
    syscall3(SYS_POWEROFF, 0, 0, 0);
    for (;;)
        ;
}
static inline long ftruncate(int fd, unsigned long size)
{
    return syscall3(SYS_FTRUNCATE, fd, (long)size, 0);
}

/* ---- strings ---- */
size_t strlen(const char *s);
int strcmp(const char *a, const char *b);
int strncmp(const char *a, const char *b, size_t n);
size_t strlcpy(char *dst, const char *src, size_t size);
char *strchr(const char *s, int c);
void *memcpy(void *dst, const void *src, size_t n);
void *memmove(void *dst, const void *src, size_t n);
void *memset(void *dst, int c, size_t n);
int memcmp(const void *a, const void *b, size_t n);
long atol(const char *s);
const char *strerror(int err); /* takes a positive errno value */

/* ---- stdio ---- */
int vdprintf(int fd, const char *fmt, va_list ap);
int dprintf(int fd, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
int printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
int puts(const char *s); /* adds a newline */
/* Read one line from stdin into buf (NUL terminated, newline removed). Returns its length, -1 at
 * EOF. */
int readline(char *buf, size_t max);

#endif
