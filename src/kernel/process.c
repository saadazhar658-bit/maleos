#include "kernel/process.h"

#include "arch/cpu.h"
#include "arch/gdt.h"
#include "arch/syscall.h"
#include "fs/vfs.h"
#include "kernel/errno.h"
#include "kernel/printk.h"
#include "kernel/random.h"
#include "kernel/spinlock.h"
#include "kernel/string.h"
#include "mm/heap.h"
#include "mm/mm.h"
#include "mm/uspace.h"
#include "mm/vmm.h"

/*
 * Processes: a user address space, a descriptor table and one kernel thread that runs the
 * program. Lifecycle:
 *
 *   process_load   build the address space (ELF + stack)        state: not launched
 *   process_launch create the thread                            state: RUNNING
 *   exit / fault   close descriptors, become a zombie           state: ZOMBIE
 *   process_wait   parent joins the thread, frees the address space and the slot
 *
 * A process whose parent exits first is an orphan; a kernel thread (the reaper) frees it
 * once it ends. proc_lock protects the table and the state fields; it is never held across
 * anything that can sleep, except through waitq_wait().
 */

static struct process *procs[PROC_MAX];
static int next_pid = 1;
static spinlock_t proc_lock = SPINLOCK_INIT;

static struct process *reap_head;
static struct waitq reap_wq;

struct process *process_current(void)
{
    return thread_current()->proc;
}

void sched_switch_hook(struct thread *next)
{
    uint64_t want = next->proc ? next->proc->pml4 : vmm_kernel_pml4();
    if (read_cr3() != want)
        write_cr3(want);
    if (next->kstack_top) {
        gdt_set_rsp0(next->kstack_top);
        syscall_kstack_top = next->kstack_top;
    }
}

/* ---------- table ---------- */

static struct process *proc_find_locked(int pid)
{
    for (int i = 0; i < PROC_MAX; i++) {
        if (procs[i] && procs[i]->pid == pid)
            return procs[i];
    }
    return NULL;
}

static void proc_free(struct process *p)
{
    uint64_t f = spin_lock_irqsave(&proc_lock);
    for (int i = 0; i < PROC_MAX; i++) {
        if (procs[i] == p)
            procs[i] = NULL;
    }
    spin_unlock_irqrestore(&proc_lock, f);
    if (p->pml4)
        vmm_space_destroy(p->pml4);
    kfree(p);
}

static struct process *proc_alloc(const char *name, const char *cwd, int ppid)
{
    struct process *p = kzalloc(sizeof(*p));
    if (!p)
        return NULL;
    p->pml4 = vmm_space_create();
    if (!p->pml4) {
        kfree(p);
        return NULL;
    }
    strlcpy(p->name, name, sizeof(p->name));
    strlcpy(p->cwd, cwd && cwd[0] ? cwd : "/", sizeof(p->cwd));
    p->ppid = ppid;
    for (int i = 0; i < 3; i++)
        p->fds[i].kind = FD_CONSOLE;

    uint64_t f = spin_lock_irqsave(&proc_lock);
    int slot = -1;
    for (int i = 0; i < PROC_MAX; i++) {
        if (!procs[i]) {
            slot = i;
            break;
        }
    }
    if (slot >= 0) {
        p->pid = next_pid++;
        procs[slot] = p;
    }
    spin_unlock_irqrestore(&proc_lock, f);

    if (slot < 0) {
        vmm_space_destroy(p->pml4);
        kfree(p);
        return NULL;
    }
    return p;
}

int process_count(void)
{
    int n = 0;
    uint64_t f = spin_lock_irqsave(&proc_lock);
    for (int i = 0; i < PROC_MAX; i++)
        n += procs[i] != NULL;
    spin_unlock_irqrestore(&proc_lock, f);
    return n;
}

int process_info(struct abi_procinfo *out, int max)
{
    int n = 0;
    uint64_t f = spin_lock_irqsave(&proc_lock);
    for (int i = 0; i < PROC_MAX && n < max; i++) {
        struct process *p = procs[i];
        if (!p || !p->launched)
            continue;
        out[n].pid = (uint32_t)p->pid;
        out[n].ppid = (uint32_t)p->ppid;
        out[n].state = (uint32_t)p->state;
        out[n].pages = p->pages;
        out[n].cpu_ticks = p->thread ? p->thread->cpu_ticks : 0;
        memset(out[n].name, 0, sizeof(out[n].name));
        strlcpy(out[n].name, p->name, sizeof(out[n].name));
        n++;
    }
    spin_unlock_irqrestore(&proc_lock, f);
    return n;
}

/* ---------- memory ---------- */

int process_map(struct process *p, uint64_t va, size_t pages, uint32_t prot)
{
    if (pages > PROC_PAGE_QUOTA || p->pages + pages > PROC_PAGE_QUOTA)
        return -ENOMEM;
    int rc = uspace_map(p->pml4, va, pages, prot);
    if (rc == 0)
        p->pages += (uint32_t)pages;
    return rc;
}

int process_sbrk(struct process *p, int64_t inc, uint64_t *old_brk)
{
    uint64_t old = p->brk;
    *old_brk = old;
    if (inc == 0)
        return 0;

    if (inc > 0 ? (uint64_t)inc > p->brk_limit - old : (uint64_t)(-inc) > old - p->brk_base)
        return -ENOMEM;
    uint64_t now = old + (uint64_t)inc;

    uint64_t old_end = ALIGN_UP(old, PAGE_SIZE), new_end = ALIGN_UP(now, PAGE_SIZE);
    if (new_end > old_end) {
        int rc = process_map(p, old_end, (new_end - old_end) / PAGE_SIZE, VMM_WRITE);
        if (rc < 0)
            return rc;
    } else if (new_end < old_end) {
        uspace_unmap(p->pml4, new_end, (old_end - new_end) / PAGE_SIZE);
        p->pages -= (uint32_t)((old_end - new_end) / PAGE_SIZE);
    }
    if (inc < 0 && now & (PAGE_SIZE - 1)) { /* the kept partial page must not leak old data */
        static const uint8_t zeros[PAGE_SIZE];
        uspace_copy_out(p->pml4, now, zeros, new_end - now);
    }
    p->brk = now;
    return 0;
}

/* ---------- descriptors ---------- */

int process_fd_install(struct process *p, enum fd_kind kind, int vfs_fd)
{
    for (int i = 3; i < PROC_FDS; i++) {
        if (p->fds[i].kind == FD_FREE) {
            p->fds[i].kind = kind;
            p->fds[i].vfs_fd = vfs_fd;
            return i;
        }
    }
    return -EMFILE;
}

struct pfile *process_fd(struct process *p, int fd)
{
    if (fd < 0 || fd >= PROC_FDS || p->fds[fd].kind == FD_FREE)
        return NULL;
    return &p->fds[fd];
}

static void close_all_fds(struct process *p)
{
    for (int i = 0; i < PROC_FDS; i++) {
        if (p->fds[i].kind == FD_VFS)
            vfs_close(p->fds[i].vfs_fd);
        p->fds[i].kind = FD_FREE;
    }
}

/* ---------- creation ---------- */

/* Copy argv onto the new stack: strings at the top, then argc, argv[] and a NULL. */
static int build_stack(struct process *p, const char *const *argv, int argc)
{
    uint64_t top = USER_STACK_TOP - (random_u64() % PROC_STACK_ASLR_PAGES) * PAGE_SIZE;
    uint64_t base = top - (uint64_t)USER_STACK_PAGES * PAGE_SIZE;
    int rc = process_map(p, base, USER_STACK_PAGES, VMM_WRITE);
    if (rc < 0)
        return rc;

    uint64_t ptrs[ABI_MAX_ARGS + 1];
    uint64_t sp = top;
    for (int i = argc - 1; i >= 0; i--) {
        size_t len = strlen(argv[i]) + 1;
        sp -= len;
        rc = uspace_copy_out(p->pml4, sp, argv[i], len);
        if (rc < 0)
            return rc;
        ptrs[i] = sp;
    }
    ptrs[argc] = 0;

    uint64_t words = 1 + (uint64_t)argc + 1;
    sp = ALIGN_DOWN(sp, 16) - ALIGN_UP(words * 8, 16);
    if (sp < base + PAGE_SIZE)
        return -E2BIG;

    uint64_t frame[1 + ABI_MAX_ARGS + 1];
    frame[0] = (uint64_t)argc;
    for (int i = 0; i <= argc; i++)
        frame[1 + i] = ptrs[i];
    rc = uspace_copy_out(p->pml4, sp, frame, words * 8);
    if (rc < 0)
        return rc;
    p->user_sp = sp;
    return 0;
}

int process_load(const void *image, size_t size, const char *name, const char *const *argv,
                 int argc, const char *cwd, int ppid, struct process **out)
{
    if (argc < 0 || argc > ABI_MAX_ARGS)
        return -E2BIG;
    struct process *p = proc_alloc(name, cwd, ppid);
    if (!p)
        return -ENOMEM;

    int rc = elf_load(p, image, size, &p->entry);
    if (rc == 0)
        rc = build_stack(p, argv, argc);
    if (rc < 0) {
        proc_free(p);
        return rc;
    }
    *out = p;
    return 0;
}

void process_discard(struct process *p)
{
    proc_free(p);
}

__attribute__((noreturn)) static void enter_user(uint64_t rip, uint64_t rsp)
{
    __asm__ volatile("pushq %0\n\t"
                     "pushq %1\n\t"
                     "pushq $0x202\n\t"
                     "pushq %2\n\t"
                     "pushq %3\n\t"
                     "xorl %%eax, %%eax\n\t"
                     "xorl %%ebx, %%ebx\n\t"
                     "xorl %%ecx, %%ecx\n\t"
                     "xorl %%edx, %%edx\n\t"
                     "xorl %%esi, %%esi\n\t"
                     "xorl %%edi, %%edi\n\t"
                     "xorl %%ebp, %%ebp\n\t"
                     "xorl %%r8d, %%r8d\n\t"
                     "xorl %%r9d, %%r9d\n\t"
                     "xorl %%r10d, %%r10d\n\t"
                     "xorl %%r11d, %%r11d\n\t"
                     "xorl %%r12d, %%r12d\n\t"
                     "xorl %%r13d, %%r13d\n\t"
                     "xorl %%r14d, %%r14d\n\t"
                     "xorl %%r15d, %%r15d\n\t"
                     "iretq"
                     :
                     : "i"(USER_DS), "r"(rsp), "i"(USER_CS), "r"(rip)
                     : "memory");
    __builtin_unreachable();
}

static void process_thread_entry(void *arg)
{
    struct process *p = arg;
    struct thread *t = thread_current();
    uint64_t f = irq_save();
    t->proc = p;
    sched_switch_hook(t); /* address space and kernel stack, before the first syscall or IRQ */
    irq_restore(f);
    enter_user(p->entry, p->user_sp);
}

int process_launch(struct process *p)
{
    struct thread *t = thread_create(p->name, process_thread_entry, p, SCHED_PRIO_NORMAL);
    if (!t)
        return -ENOMEM;
    uint64_t f = spin_lock_irqsave(&proc_lock);
    p->thread = t;
    if (p->state == 0)
        p->state = PROC_RUNNING; /* it may already have run and exited */
    p->launched = true;
    spin_unlock_irqrestore(&proc_lock, f);
    return p->pid;
}

int process_spawn(const char *path, const char *const *argv, int argc, const char *stdout_path,
                  int stdout_flags, const char *cwd, int ppid)
{
    struct stat st;
    int rc = vfs_stat(path, &st);
    if (rc < 0)
        return rc;
    if (!S_ISREG(st.mode))
        return -EACCES;
    if (st.size == 0 || st.size > ELF_MAX_SIZE)
        return -ENOEXEC;

    uint8_t *image = kmalloc(st.size);
    if (!image)
        return -ENOMEM;
    int64_t n = vfs_read_file(path, image, st.size);
    if (n != (int64_t)st.size) {
        kfree(image);
        return n < 0 ? (int)n : -EIO;
    }

    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    struct process *p;
    rc = process_load(image, st.size, base, argv, argc, cwd, ppid, &p);
    kfree(image);
    if (rc < 0)
        return rc;

    if (stdout_path) {
        int gfd = vfs_open(stdout_path, O_WRONLY | O_CREAT | (stdout_flags & (O_TRUNC | O_APPEND)));
        if (gfd < 0) {
            proc_free(p);
            return gfd;
        }
        p->fds[1].kind = FD_VFS;
        p->fds[1].vfs_fd = gfd;
    }

    rc = process_launch(p);
    if (rc < 0) {
        close_all_fds(p);
        proc_free(p);
    }
    return rc;
}

/* ---------- exit, wait, reaping ---------- */

static void reap(struct process *p)
{
    if (p->thread)
        thread_join(p->thread); /* waits until the thread has really stopped running */
    proc_free(p);
}

static void reaper_entry(void *arg)
{
    (void)arg;
    for (;;) {
        uint64_t f = spin_lock_irqsave(&proc_lock);
        while (!reap_head)
            waitq_wait(&reap_wq, &proc_lock, 0);
        struct process *p = reap_head;
        reap_head = p->reap_next;
        spin_unlock_irqrestore(&proc_lock, f);
        reap(p);
    }
}

void process_init(void)
{
    waitq_init(&reap_wq);
    thread_detach(thread_create("reaper", reaper_entry, NULL, SCHED_PRIO_NORMAL));
}

static void queue_for_reaping_locked(struct process *p)
{
    p->reap_next = reap_head;
    reap_head = p;
    waitq_wake_one(&reap_wq);
}

void process_exit(int code)
{
    struct process *p = process_current();
    if (!p)
        kpanic("process_exit from a kernel thread");

    close_all_fds(p);

    uint64_t f = spin_lock_irqsave(&proc_lock);
    p->exit_code = code;
    p->state = PROC_ZOMBIE;
    for (int i = 0; i < PROC_MAX; i++) {
        struct process *q = procs[i];
        if (q && q->ppid == p->pid && q != p) {
            q->ppid = 0;
            q->orphan = true;
            if (q->state == PROC_ZOMBIE && !q->waited) {
                q->waited = true;
                queue_for_reaping_locked(q);
            }
        }
    }
    if (p->orphan && !p->waited) {
        p->waited = true;
        queue_for_reaping_locked(p);
    }
    spin_unlock_irqrestore(&proc_lock, f);

    thread_exit(code);
}

int process_wait(int pid, int ppid, int *status)
{
    uint64_t f = spin_lock_irqsave(&proc_lock);
    struct process *p = proc_find_locked(pid);
    int rc = 0;
    if (!p)
        rc = -ESRCH;
    else if (p->ppid != ppid || p->waited || !p->launched)
        rc = -ECHILD;
    else
        p->waited = true;
    spin_unlock_irqrestore(&proc_lock, f);
    if (rc < 0)
        return rc;

    int code = thread_join(p->thread);
    p->thread = NULL;
    if (status)
        *status = code;
    proc_free(p);
    return pid;
}

/* ---------- faults ---------- */

void process_user_fault(struct interrupt_frame *f)
{
    irq_enable(); /* the frame is saved; from here on this is an ordinary kernel context */

    struct process *p = process_current();
    int code;
    const char *what;
    switch (f->vector) {
    case 0:
        code = EXIT_SIGFPE;
        what = "divide error";
        break;
    case 6:
        code = EXIT_SIGILL;
        what = "invalid instruction";
        break;
    case 14:
        code = EXIT_SIGSEGV;
        what = "page fault";
        break;
    case 13:
        code = EXIT_SIGSEGV;
        what = "general protection fault";
        break;
    case 3:
    case 1:
        code = EXIT_SIGTRAP;
        what = "trap";
        break;
    default:
        code = EXIT_SIGILL;
        what = "CPU exception";
        break;
    }

    if (f->vector == 14) {
        uint64_t e = f->error;
        printk("[pid %d %s] killed: %s at rip=%lx accessing %lx (%s, %s)\n", p ? p->pid : -1,
               p ? p->name : "?", what, (unsigned long)f->rip, (unsigned long)read_cr2(),
               (e & 1) ? "protection" : "not mapped",
               (e & 2)    ? "write"
               : (e & 16) ? "exec"
                          : "read");
    } else {
        printk("[pid %d %s] killed: %s (vector %lu) at rip=%lx\n", p ? p->pid : -1,
               p ? p->name : "?", what, (unsigned long)f->vector, (unsigned long)f->rip);
    }
    process_exit(code);
}
