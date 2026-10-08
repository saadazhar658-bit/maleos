#ifndef MALEOS_MM_USPACE_H
#define MALEOS_MM_USPACE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Safe access to user memory. The kernel never dereferences a user pointer: each page is
 * looked up in the process's page tables (it must be present, user-accessible and, for
 * writes, writable) and the bytes are copied through the kernel's direct map. A bad pointer
 * therefore produces -EFAULT instead of a fault, and SMAP can stay enabled.
 */

/* User addresses are below this (the lower canonical half). */
#define USER_TOP 0x0000800000000000ULL

bool user_range_ok(uint64_t va, uint64_t len);

int uspace_copy_in(uint64_t pml4, void *dst, uint64_t src_va, size_t n);        /* user -> kernel */
int uspace_copy_out(uint64_t pml4, uint64_t dst_va, const void *src, size_t n); /* kernel -> user */
/* True if [va, va+n) is mapped user memory the process may write. */
bool uspace_writable(uint64_t pml4, uint64_t va, size_t n);
/* Copy a NUL-terminated string (at most max-1 characters). Returns its length, -EFAULT or
 * -ENAMETOOLONG. */
int uspace_strcpy_in(uint64_t pml4, char *dst, uint64_t src_va, size_t max);

/* Map `pages` zero-filled pages at va (all or nothing). */
int uspace_map(uint64_t pml4, uint64_t va, size_t pages, uint32_t prot);
/* Unmap and free `pages` pages starting at va; unmapped holes are skipped. */
void uspace_unmap(uint64_t pml4, uint64_t va, size_t pages);

#endif
