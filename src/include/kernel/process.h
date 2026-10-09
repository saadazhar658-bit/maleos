#ifndef MALEOS_KERNEL_PROCESS_H
#define MALEOS_KERNEL_PROCESS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "abi/abi.h"
#include "arch/idt.h"
#include "kernel/sched.h"

#define PROC_MAX 32
#define PROC_FDS 16

/* User address space layout (see docs/USERLAND.md). */
#define USER_IMAGE_MIN 0x10000ULL            /* nothing below this is ever mapped: NULL faults */
#define USER_IMAGE_MAX 0x00007FFF00000000ULL /* the ELF image must end below this */
#define USER_STACK_TOP 0x00007FFFFFFFF000ULL /* the page above the stack stays unmapped */
#define USER_STACK_PAGES 16                  /* 64 KiB; the page below is an unmapped guard */
#define PROC_PAGE_QUOTA 4096                 /* at most 16 MiB of user pages per process */
#define PROC_HEAP_ASLR_PAGES 256             /* heap start slides by up to this many pages */
#define PROC_STACK_ASLR_PAGES 512            /* stack top slides down by up to this many pages */
#define PROC_HEAP_PAGES 2048                 /* sbrk limit: 8 MiB */
#define ELF_MAX_SIZE (4u << 20)

enum fd_kind { FD_FREE = 0, FD_CONSOLE, FD_VFS };

struct pfile {
    enum fd_kind kind;
    int vfs_fd; /* descriptor in the global VFS table when kind == FD_VFS */
};

struct process {
    int pid;
    int ppid; /* 0 = the kernel */
    char name[24];
    int state; /* PROC_RUNNING / PROC_ZOMBIE (abi.h); 0 until launched */
    bool launched;
    bool waited; /* a parent is already waiting for it */
    bool orphan; /* its parent is gone: the reaper cleans up after it */
    uint64_t pml4;
    struct thread *thread;
    int exit_code;
    struct pfile fds[PROC_FDS];
    char cwd[VFS_PATH_MAX];
    uint64_t entry;
    uint64_t user_sp;
    uint64_t brk_base, brk, brk_limit;
    uint32_t pages; /* user pages owned (checked against PROC_PAGE_QUOTA) */
    struct process *reap_next;
};

void process_init(void); /* start the reaper thread */
struct process *process_current(void);

/*
 * Build a process from an ELF image without running it: address space, segments, stack with
 * argv, descriptors 0-2 on the console. The image is copied; the caller keeps ownership.
 */
int process_load(const void *image, size_t size, const char *name, const char *const *argv,
                 int argc, const char *cwd, int ppid, struct process **out);
int process_launch(struct process *p);   /* start its thread; returns the pid or -errno */
void process_discard(struct process *p); /* free a process that was loaded but never launched */

/* Load /path from the VFS and start it. stdout_path may be NULL (console). Returns the pid or
 * -errno. */
int process_spawn(const char *path, const char *const *argv, int argc, const char *stdout_path,
                  int stdout_flags, const char *cwd, int ppid);

/* Wait for child `pid` of `ppid` (0 = the kernel); frees it. Returns pid, -ECHILD or -ESRCH. */
int process_wait(int pid, int ppid, int *status);

__attribute__((noreturn)) void process_exit(int code);
__attribute__((noreturn)) void process_user_fault(struct interrupt_frame *f);

int process_map(struct process *p, uint64_t va, size_t pages, uint32_t prot);
int process_sbrk(struct process *p, int64_t increment, uint64_t *old_brk);

int process_fd_install(struct process *p, enum fd_kind kind, int vfs_fd);
struct pfile *process_fd(struct process *p, int fd);

int process_count(void);
int process_info(struct abi_procinfo *out, int max);

/* ELF loader (elf.c). Fills p's address space; returns the entry point. */
int elf_load(struct process *p, const void *image, size_t size, uint64_t *entry);

#endif
