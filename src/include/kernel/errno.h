#ifndef MALEOS_KERNEL_ERRNO_H
#define MALEOS_KERNEL_ERRNO_H

/* Error numbers (Linux values). Kernel functions return them negated: -ENOENT. */
#define EPERM 1
#define ENOENT 2
#define EIO 5
#define ENXIO 6
#define EBADF 9
#define ENOMEM 12
#define EACCES 13
#define EBUSY 16
#define EEXIST 17
#define EXDEV 18
#define ENODEV 19
#define ENOTDIR 20
#define EISDIR 21
#define EINVAL 22
#define EMFILE 24
#define EFBIG 27
#define ENOSPC 28
#define ESPIPE 29
#define EROFS 30
#define ENAMETOOLONG 36
#define ENOSYS 38
#define ENOTEMPTY 39
#define ELOOP 40

#endif
