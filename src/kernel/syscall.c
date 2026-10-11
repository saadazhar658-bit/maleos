#include "arch/syscall.h"

#include "arch/cpu.h"
#include "arch/idt.h"
#include "arch/power.h"
#include "fs/vfs.h"
#include "kernel/console.h"
#include "kernel/errno.h"
#include "kernel/printk.h"
#include "kernel/process.h"
#include "kernel/random.h"
#include "kernel/sched.h"
#include "kernel/string.h"
#include "mm/pmm.h"
#include "mm/uspace.h"
#include "net/net.h"

/* 32-bit arguments (descriptors, pids, flags): a 64-bit value that does not fit is invalid,
 * not silently truncated (1<<32 must not alias descriptor 0). */
#define ARG_INT(x) (((int64_t)(x) == (int64_t)(int)(x)) ? (int)(x) : -1)

/*
 * System call dispatcher. Every pointer argument is validated and copied with the uspace_*
 * helpers; nothing here dereferences user memory. Results are 64-bit: >= 0 on success,
 * -errno on failure.
 */

#define IO_CHUNK 2048
#define OPEN_FLAGS_ALLOWED                                                                         \
    (O_ACCMODE | O_CREAT | O_EXCL | O_TRUNC | O_APPEND | O_DIRECTORY | O_NOFOLLOW)

typedef int64_t sysret;

/* Resolve a user path (relative ones against the process's directory) into `out`. */
static int get_path(struct process *p, uint64_t uptr, char *out)
{
    char raw[VFS_PATH_MAX];
    int n = uspace_strcpy_in(p->pml4, raw, uptr, sizeof(raw));
    if (n < 0)
        return n;
    if (n == 0)
        return -ENOENT;
    if (raw[0] == '/') {
        memcpy(out, raw, (size_t)n + 1);
        return 0;
    }
    size_t cl = strlen(p->cwd);
    bool slash = cl > 0 && p->cwd[cl - 1] == '/';
    if (cl + (slash ? 0 : 1) + (size_t)n >= VFS_PATH_MAX)
        return -ENAMETOOLONG;
    memcpy(out, p->cwd, cl);
    if (!slash)
        out[cl++] = '/';
    memcpy(out + cl, raw, (size_t)n + 1);
    return 0;
}

/* Collapse "." and ".." textually, for the stored working directory. */
static void normalize(const char *in, char *out)
{
    size_t len = 0;
    out[0] = '/';
    out[1] = 0;
    len = 1;
    const char *s = in;
    while (*s) {
        while (*s == '/')
            s++;
        if (!*s)
            break;
        const char *e = s;
        while (*e && *e != '/')
            e++;
        size_t n = (size_t)(e - s);
        if (n == 1 && s[0] == '.') {
            /* skip */
        } else if (n == 2 && s[0] == '.' && s[1] == '.') {
            while (len > 1 && out[len - 1] != '/')
                len--;
            if (len > 1)
                len--; /* drop the slash before the removed component */
            out[len] = 0;
        } else {
            if (len > 1)
                out[len++] = '/';
            memcpy(out + len, s, n);
            len += n;
            out[len] = 0;
        }
        s = e;
    }
}

static sysret sys_write(struct process *p, int fd, uint64_t ubuf, uint64_t len)
{
    struct pfile *pf = process_fd(p, fd);
    if (!pf)
        return -EBADF;
    char buf[IO_CHUNK];
    uint64_t done = 0;
    while (done < len) {
        size_t chunk = len - done < IO_CHUNK ? (size_t)(len - done) : IO_CHUNK;
        int rc = uspace_copy_in(p->pml4, buf, ubuf + done, chunk);
        if (rc < 0)
            return done ? (sysret)done : rc;
        if (pf->kind == FD_CONSOLE) {
            console_write(buf, chunk);
            done += chunk;
        } else if (pf->kind == FD_SOCK) {
            int64_t w = sock_send(pf->vfs_fd, buf, chunk, 0, 0);
            if (w < 0)
                return done ? (sysret)done : w;
            done += (uint64_t)w;
        } else {
            int64_t w = vfs_write(pf->vfs_fd, buf, chunk);
            if (w < 0)
                return done ? (sysret)done : w;
            done += (uint64_t)w;
            if ((size_t)w < chunk)
                break;
        }
    }
    return (sysret)done;
}

static sysret sys_read(struct process *p, int fd, uint64_t ubuf, uint64_t len)
{
    struct pfile *pf = process_fd(p, fd);
    if (!pf)
        return -EBADF;
    char buf[IO_CHUNK];
    uint64_t done = 0;
    while (done < len) {
        size_t chunk = len - done < IO_CHUNK ? (size_t)(len - done) : IO_CHUNK;
        if (!uspace_writable(p->pml4, ubuf + done, chunk)) /* before blocking on the console */
            return done ? (sysret)done : -EFAULT;
        int64_t r;
        if (pf->kind == FD_CONSOLE)
            r = console_read(buf, chunk);
        else if (pf->kind == FD_SOCK)
            r = sock_recv(pf->vfs_fd, buf, chunk, NULL, NULL);
        else
            r = vfs_read(pf->vfs_fd, buf, chunk);
        if (r < 0)
            return done ? (sysret)done : r;
        if (r == 0)
            break;
        int rc = uspace_copy_out(p->pml4, ubuf + done, buf, (size_t)r);
        if (rc < 0)
            return done ? (sysret)done : rc;
        done += (uint64_t)r;
        if (pf->kind == FD_CONSOLE || pf->kind == FD_SOCK || (size_t)r < chunk)
            break; /* a terminal or socket read returns what is available */
    }
    return (sysret)done;
}

static sysret sys_open(struct process *p, uint64_t upath, uint64_t flags)
{
    if (flags & ~(uint64_t)OPEN_FLAGS_ALLOWED)
        return -EINVAL;
    char path[VFS_PATH_MAX];
    int rc = get_path(p, upath, path);
    if (rc < 0)
        return rc;
    int gfd = vfs_open(path, (int)flags);
    if (gfd < 0)
        return gfd;
    int fd = process_fd_install(p, FD_VFS, gfd);
    if (fd < 0)
        vfs_close(gfd);
    return fd;
}

static sysret sys_close(struct process *p, int fd)
{
    struct pfile *pf = process_fd(p, fd);
    if (!pf)
        return -EBADF;
    if (pf->kind == FD_VFS)
        vfs_close(pf->vfs_fd);
    else if (pf->kind == FD_SOCK)
        sock_close(pf->vfs_fd);
    pf->kind = FD_FREE;
    return 0;
}

static struct pfile *vfs_file(struct process *p, int fd)
{
    struct pfile *pf = process_fd(p, fd);
    return pf && pf->kind == FD_VFS ? pf : NULL;
}

static sysret sys_stat(struct process *p, uint64_t upath, uint64_t ust, bool follow)
{
    char path[VFS_PATH_MAX];
    int rc = get_path(p, upath, path);
    if (rc < 0)
        return rc;
    struct stat st;
    rc = follow ? vfs_stat(path, &st) : vfs_lstat(path, &st);
    if (rc < 0)
        return rc;
    return uspace_copy_out(p->pml4, ust, &st, sizeof(st));
}

static sysret sys_path_op(struct process *p, int nr, uint64_t upath)
{
    char path[VFS_PATH_MAX];
    int rc = get_path(p, upath, path);
    if (rc < 0)
        return rc;
    switch (nr) {
    case SYS_MKDIR:
        return vfs_mkdir(path);
    case SYS_RMDIR:
        return vfs_rmdir(path);
    default:
        return vfs_unlink(path);
    }
}

static sysret sys_chdir(struct process *p, uint64_t upath)
{
    char path[VFS_PATH_MAX];
    int rc = get_path(p, upath, path);
    if (rc < 0)
        return rc;
    struct stat st;
    rc = vfs_stat(path, &st);
    if (rc < 0)
        return rc;
    if (!S_ISDIR(st.mode))
        return -ENOTDIR;
    normalize(path, p->cwd);
    return 0;
}

static sysret sys_spawn(struct process *p, uint64_t upath, uint64_t uargv, uint64_t uattr)
{
    char path[VFS_PATH_MAX];
    int rc = get_path(p, upath, path);
    if (rc < 0)
        return rc;

    _Static_assert(ABI_MAX_ARGS * ABI_ARG_MAX <= 4096, "argument buffer too large for the stack");
    char args[ABI_MAX_ARGS][ABI_ARG_MAX];
    const char *argv[ABI_MAX_ARGS];
    int argc = 0;
    if (uargv) {
        for (;; argc++) {
            if (argc == ABI_MAX_ARGS)
                return -E2BIG;
            uint64_t ptr;
            rc = uspace_copy_in(p->pml4, &ptr, uargv + (uint64_t)argc * 8, sizeof(ptr));
            if (rc < 0)
                return rc;
            if (ptr == 0)
                break;
            rc = uspace_strcpy_in(p->pml4, args[argc], ptr, ABI_ARG_MAX);
            if (rc == -ENAMETOOLONG)
                return -E2BIG;
            if (rc < 0)
                return rc;
            argv[argc] = args[argc];
        }
    }
    if (argc == 0) {
        strlcpy(args[0], path, ABI_ARG_MAX);
        argv[0] = args[0];
        argc = 1;
    }

    char out_path[VFS_PATH_MAX];
    const char *out = NULL;
    int out_flags = 0;
    if (uattr) {
        struct abi_spawn_attr attr;
        rc = uspace_copy_in(p->pml4, &attr, uattr, sizeof(attr));
        if (rc < 0)
            return rc;
        if (attr.stdout_flags & ~(O_TRUNC | O_APPEND))
            return -EINVAL;
        if (attr.stdout_path) {
            rc = get_path(p, (uint64_t)attr.stdout_path, out_path);
            if (rc < 0)
                return rc;
            out = out_path;
            out_flags = attr.stdout_flags;
        }
    }
    return process_spawn(path, argv, argc, out, out_flags, p->cwd, p->pid);
}

static sysret sys_wait(struct process *p, int pid, uint64_t ustatus)
{
    /* Fail before reaping rather than lose the result; a probe must not write to the buffer. */
    if (ustatus && !uspace_writable(p->pml4, ustatus, sizeof(int)))
        return -EFAULT;
    int status = 0;
    int rc = process_wait(pid, p->pid, &status);
    if (rc < 0)
        return rc;
    if (ustatus)
        uspace_copy_out(p->pml4, ustatus, &status, sizeof(status));
    return rc;
}

static sysret sys_readdir(struct process *p, int fd, uint64_t uent)
{
    struct pfile *pf = vfs_file(p, fd);
    if (!pf)
        return -EBADF;
    struct dirent de;
    int rc = vfs_readdir(pf->vfs_fd, &de);
    if (rc <= 0)
        return rc;
    int c = uspace_copy_out(p->pml4, uent, &de, sizeof(de));
    return c < 0 ? c : 1;
}

static sysret sys_procinfo(struct process *p, uint64_t ubuf, uint64_t max)
{
    if (max > PROC_MAX)
        max = PROC_MAX;
    struct abi_procinfo info[PROC_MAX];
    int n = process_info(info, (int)max);
    int rc = uspace_copy_out(p->pml4, ubuf, info, (size_t)n * sizeof(info[0]));
    return rc < 0 ? rc : n;
}

static sysret sys_getrandom(struct process *p, uint64_t ubuf, uint64_t len)
{
    if (len > ABI_RANDOM_MAX)
        return -EINVAL;
    uint8_t buf[ABI_RANDOM_MAX];
    random_bytes(buf, len);
    int rc = uspace_copy_out(p->pml4, ubuf, buf, len);
    memset(buf, 0, sizeof(buf));
    return rc < 0 ? rc : (sysret)len;
}

/* ---------- sockets ---------- */

static int sock_of(struct process *p, int fd)
{
    struct pfile *pf = process_fd(p, fd);
    if (!pf)
        return -EBADF;
    return pf->kind == FD_SOCK ? pf->vfs_fd : -ENOTSOCK;
}

static int install_socket(struct process *p, int s)
{
    int fd = process_fd_install(p, FD_SOCK, s);
    if (fd < 0)
        sock_close(s);
    return fd;
}

static sysret sys_socket(struct process *p, int type)
{
    int s = sock_create(type);
    return s < 0 ? s : install_socket(p, s);
}

static sysret get_addr(struct process *p, uint64_t uaddr, struct abi_sockaddr *out)
{
    return uspace_copy_in(p->pml4, out, uaddr, sizeof(*out));
}

static sysret sys_bind(struct process *p, int fd, uint64_t uaddr)
{
    int s = sock_of(p, fd);
    struct abi_sockaddr a;
    if (s < 0)
        return s;
    int rc = (int)get_addr(p, uaddr, &a);
    return rc < 0 ? rc : sock_bind(s, a.ip, a.port);
}

static sysret sys_connect(struct process *p, int fd, uint64_t uaddr)
{
    int s = sock_of(p, fd);
    struct abi_sockaddr a;
    if (s < 0)
        return s;
    int rc = (int)get_addr(p, uaddr, &a);
    return rc < 0 ? rc : sock_connect(s, a.ip, a.port);
}

static sysret sys_accept(struct process *p, int fd, uint64_t uaddr)
{
    int s = sock_of(p, fd);
    if (s < 0)
        return s;
    if (uaddr && !uspace_writable(p->pml4, uaddr, sizeof(struct abi_sockaddr)))
        return -EFAULT;
    struct abi_sockaddr a = {0};
    int ns = sock_accept(s, &a.ip, &a.port);
    if (ns < 0)
        return ns;
    int nfd = install_socket(p, ns);
    if (nfd >= 0 && uaddr)
        uspace_copy_out(p->pml4, uaddr, &a, sizeof(a));
    return nfd;
}

#define NET_CHUNK 2048

static sysret sys_sendto(struct process *p, int fd, uint64_t udg)
{
    int s = sock_of(p, fd);
    struct abi_dgram dg;
    if (s < 0)
        return s;
    int rc = uspace_copy_in(p->pml4, &dg, udg, sizeof(dg));
    if (rc < 0)
        return rc;
    uint8_t buf[NET_CHUNK];
    uint64_t done = 0;
    uint64_t ubuf = (uint64_t)dg.buf;
    bool first = true;
    while (first || done < dg.len) {
        size_t chunk = dg.len - done < NET_CHUNK ? (size_t)(dg.len - done) : NET_CHUNK;
        rc = uspace_copy_in(p->pml4, buf, ubuf + done, chunk);
        if (rc < 0)
            return done ? (sysret)done : rc;
        int64_t w = sock_send(s, buf, chunk, dg.addr.ip, dg.addr.port);
        if (w < 0)
            return done ? (sysret)done : w;
        done += (uint64_t)w;
        first = false;
        if ((size_t)w < chunk)
            break;
    }
    return (sysret)done;
}

static sysret sys_recvfrom(struct process *p, int fd, uint64_t udg)
{
    int s = sock_of(p, fd);
    struct abi_dgram dg;
    if (s < 0)
        return s;
    int rc = uspace_copy_in(p->pml4, &dg, udg, sizeof(dg));
    if (rc < 0)
        return rc;
    size_t want = dg.len < NET_CHUNK ? (size_t)dg.len : NET_CHUNK;
    if (!uspace_writable(p->pml4, (uint64_t)dg.buf, want) ||
        !uspace_writable(p->pml4, udg, sizeof(dg))) /* before blocking */
        return -EFAULT;
    uint8_t buf[NET_CHUNK];
    struct abi_sockaddr from = {0};
    int64_t n = sock_recv(s, buf, want, &from.ip, &from.port);
    if (n < 0)
        return n;
    rc = uspace_copy_out(p->pml4, (uint64_t)dg.buf, buf, (size_t)n);
    if (rc < 0)
        return rc;
    dg.addr = from;
    uspace_copy_out(p->pml4, udg, &dg, sizeof(dg));
    return n;
}

static sysret sys_sockctl(struct process *p, int fd, uint64_t op, uint64_t value)
{
    int s = sock_of(p, fd);
    if (s < 0)
        return s;
    int64_t out = 0;
    int rc = sock_ctl(s, ARG_INT(op), (int64_t)value, &out);
    return rc < 0 ? rc : out;
}

static sysret sys_netinfo(struct process *p, uint64_t ubuf)
{
    struct abi_netinfo ni;
    net_get_info(&ni);
    int rc = uspace_copy_out(p->pml4, ubuf, &ni, sizeof(ni));
    return rc < 0 ? rc : 0;
}

static sysret sys_ping(uint64_t ip, uint64_t timeout_ms, uint64_t seq)
{
    if (ip > 0xffffffffULL || timeout_ms > 60000)
        return -EINVAL;
    return net_ping((uint32_t)ip, (uint32_t)timeout_ms, (uint16_t)seq);
}

static sysret sys_meminfo(struct process *p, uint64_t ubuf)
{
    struct abi_meminfo mi = {
        .page_size = 4096,
        .total_frames = pmm_total_frames(),
        .free_frames = pmm_free_frame_count(),
        .processes = (uint32_t)process_count(),
    };
    return uspace_copy_out(p->pml4, ubuf, &mi, sizeof(mi));
}

static sysret do_syscall(struct process *p, uint64_t nr, uint64_t a0, uint64_t a1, uint64_t a2)
{
    switch (nr) {
    case SYS_EXIT:
        process_exit((int)(a0 & 0xFF));
    case SYS_WRITE:
        return sys_write(p, ARG_INT(a0), a1, a2);
    case SYS_READ:
        return sys_read(p, ARG_INT(a0), a1, a2);
    case SYS_OPEN:
        return sys_open(p, a0, a1);
    case SYS_CLOSE:
        return sys_close(p, ARG_INT(a0));
    case SYS_LSEEK: {
        struct pfile *pf = vfs_file(p, ARG_INT(a0));
        return pf ? vfs_lseek(pf->vfs_fd, (int64_t)a1, ARG_INT(a2)) : -EBADF;
    }
    case SYS_STAT:
        return sys_stat(p, a0, a1, true);
    case SYS_LSTAT:
        return sys_stat(p, a0, a1, false);
    case SYS_FSTAT: {
        struct pfile *pf = vfs_file(p, ARG_INT(a0));
        if (!pf)
            return -EBADF;
        struct stat st;
        int rc = vfs_fstat(pf->vfs_fd, &st);
        return rc < 0 ? rc : uspace_copy_out(p->pml4, a1, &st, sizeof(st));
    }
    case SYS_READDIR:
        return sys_readdir(p, ARG_INT(a0), a1);
    case SYS_MKDIR:
    case SYS_RMDIR:
    case SYS_UNLINK:
        return sys_path_op(p, (int)nr, a0);
    case SYS_SYMLINK: {
        char target[VFS_PATH_MAX], path[VFS_PATH_MAX];
        int rc = uspace_strcpy_in(p->pml4, target, a0, sizeof(target));
        if (rc < 0)
            return rc;
        rc = get_path(p, a1, path);
        return rc < 0 ? rc : vfs_symlink(target, path);
    }
    case SYS_READLINK: {
        char path[VFS_PATH_MAX], buf[VFS_PATH_MAX];
        int rc = get_path(p, a0, path);
        if (rc < 0)
            return rc;
        if (a2 > sizeof(buf))
            a2 = sizeof(buf);
        rc = vfs_readlink(path, buf, (size_t)a2);
        if (rc < 0)
            return rc;
        int c = uspace_copy_out(p->pml4, a1, buf, (size_t)rc);
        return c < 0 ? c : rc;
    }
    case SYS_CHDIR:
        return sys_chdir(p, a0);
    case SYS_GETCWD: {
        size_t need = strlen(p->cwd) + 1;
        if (a1 < need)
            return -ERANGE;
        int rc = uspace_copy_out(p->pml4, a0, p->cwd, need);
        return rc < 0 ? rc : (sysret)need;
    }
    case SYS_GETPID:
        return p->pid;
    case SYS_YIELD:
        sched_yield();
        return 0;
    case SYS_SLEEP_MS:
        thread_sleep_ms(a0 > 60000 ? 60000 : a0);
        return 0;
    case SYS_UPTIME_MS:
        return (sysret)(timer_ticks() * 1000 / TIMER_HZ);
    case SYS_SPAWN:
        return sys_spawn(p, a0, a1, a2);
    case SYS_WAIT:
        return sys_wait(p, ARG_INT(a0), a1);
    case SYS_SBRK: {
        uint64_t old;
        int rc = process_sbrk(p, (int64_t)a0, &old);
        return rc < 0 ? rc : (sysret)old;
    }
    case SYS_PROCINFO:
        return sys_procinfo(p, a0, a1);
    case SYS_MEMINFO:
        return sys_meminfo(p, a0);
    case SYS_POWEROFF:
        machine_poweroff();
    case SYS_SOCKET:
        return sys_socket(p, ARG_INT(a0));
    case SYS_BIND:
        return sys_bind(p, ARG_INT(a0), a1);
    case SYS_LISTEN: {
        int sd = sock_of(p, ARG_INT(a0));
        return sd < 0 ? sd : sock_listen(sd, ARG_INT(a1));
    }
    case SYS_ACCEPT:
        return sys_accept(p, ARG_INT(a0), a1);
    case SYS_CONNECT:
        return sys_connect(p, ARG_INT(a0), a1);
    case SYS_SENDTO:
        return sys_sendto(p, ARG_INT(a0), a1);
    case SYS_RECVFROM:
        return sys_recvfrom(p, ARG_INT(a0), a1);
    case SYS_SOCKCTL:
        return sys_sockctl(p, ARG_INT(a0), a1, a2);
    case SYS_NETINFO:
        return sys_netinfo(p, a0);
    case SYS_PING:
        return sys_ping(a0, a1, a2);
    case SYS_GETRANDOM:
        return sys_getrandom(p, a0, a1);
    case SYS_FTRUNCATE: {
        struct pfile *pf = vfs_file(p, ARG_INT(a0));
        return pf ? vfs_ftruncate(pf->vfs_fd, a1) : -EBADF;
    }
    default:
        return -ENOSYS;
    }
}

void syscall_dispatch(struct interrupt_frame *f)
{
    struct process *p = process_current();
    if (!p)
        kpanic("system call from a thread without a process");
    f->rax = (uint64_t)do_syscall(p, f->rax, f->rdi, f->rsi, f->rdx);
}
