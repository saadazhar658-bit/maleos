#ifndef MALEOS_KERNEL_ERRNO_H
#define MALEOS_KERNEL_ERRNO_H

/* Error numbers (Linux values). Kernel functions return them negated: -ENOENT. */
#define EPERM 1
#define ENOENT 2
#define ESRCH 3
#define E2BIG 7
#define ENOEXEC 8
#define ECHILD 10
#define EIO 5
#define ENXIO 6
#define EBADF 9
#define EAGAIN 11
#define ENOMEM 12
#define EACCES 13
#define EFAULT 14
#define EBUSY 16
#define EEXIST 17
#define EXDEV 18
#define ENODEV 19
#define ENOTDIR 20
#define EISDIR 21
#define EINVAL 22
#define EMFILE 24
#define EFBIG 27
#define ERANGE 34
#define ENOSPC 28
#define ESPIPE 29
#define EPIPE 32
#define EROFS 30
#define ENAMETOOLONG 36
#define ENOSYS 38
#define ENOTEMPTY 39
#define ELOOP 40
#define ENOTSOCK 88
#define EDESTADDRREQ 89
#define EMSGSIZE 90
#define EPROTONOSUPPORT 93
#define EOPNOTSUPP 95
#define EADDRINUSE 98
#define ENETUNREACH 101
#define ECONNABORTED 103
#define ECONNRESET 104
#define EISCONN 106
#define ENOTCONN 107
#define ETIMEDOUT 110
#define ECONNREFUSED 111
#define EHOSTUNREACH 113

#endif
