#ifndef MALEOS_KERNEL_PIPE_H
#define MALEOS_KERNEL_PIPE_H

#include <stdint.h>

#define PIPE_MAX 32
#define PIPE_BUF_SIZE 4096
#define PIPE_END_READ 0
#define PIPE_END_WRITE 1

/* Create a pipe with one reference on each end. Returns its id or -ENFILE / -ENOMEM. */
int pipe_create(void);
/* Another descriptor now refers to this end (dup, spawn inheritance). */
void pipe_ref(int id, int end);
/* A descriptor for this end was closed; the pipe is freed when both ends have no users. */
void pipe_close(int id, int end);
/* Blocks until data is available; 0 means end of file (no writers left). */
int64_t pipe_read(int id, void *buf, uint64_t len);
/* Blocks while the buffer is full; writes everything unless the last reader goes away, then
 * returns what was written or -EPIPE. */
int64_t pipe_write(int id, const void *buf, uint64_t len);
int pipe_count(void); /* pipes in use (leak checks) */

#endif
