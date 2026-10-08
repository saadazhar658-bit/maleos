#include "ulib.h"

/* Throws invalid arguments at every system call. The kernel must answer with an error each time. */

#define KERNEL_PTR 0xFFFF800000000000ULL
#define UNMAPPED 0x10000000UL
#define STACK_TOP 0x00007FFFFFFFF000UL

static int failures;
static int check_no;

static void expect(long got, long want)
{
    check_no++;
    if (got != want) {
        dprintf(2, "t_sys: check %d: got %ld, want %ld\n", check_no, got, want);
        failures++;
    }
}

int main(void)
{
    char buf[64];
    struct stat st;

    /* Unknown system calls. */
    expect(syscall3(-1, 0, 0, 0), -ENOSYS);
    expect(syscall3(SYS_COUNT, 0, 0, 0), -ENOSYS);
    expect(syscall3(1000, 0, 0, 0), -ENOSYS);
    expect(syscall3(0x7FFFFFFFFFFFFFFFL, 0, 0, 0), -ENOSYS);

    /* Bad descriptors. */
    expect(write(-1, "x", 1), -EBADF);
    expect(write(16, "x", 1), -EBADF);
    expect(write(99, "x", 1), -EBADF);
    expect(read(7, buf, 1), -EBADF);
    expect(close(5), -EBADF);
    expect(lseek(0, 0, SEEK_SET), -EBADF); /* the console is not seekable */
    expect(fstat(9, &st), -EBADF);
    expect(readdir(9, (struct dirent *)buf), -EBADF);
    expect(ftruncate(0, 0), -EBADF);

    /* Bad pointers: NULL, unmapped, kernel, wrap-around, straddling the end of the mapping. */
    expect(write(1, (void *)0, 8), -EFAULT);
    expect(write(1, (void *)UNMAPPED, 8), -EFAULT);
    expect(write(1, (void *)KERNEL_PTR, 8), -EFAULT);
    expect(write(1, (void *)0xFFFFFFFFFFFFF000ULL, 0x2000), -EFAULT);
    expect(write(1, (void *)0x00007FFFFFFFF000UL, 8), -EFAULT);
    expect(syscall3(SYS_READ, 0, KERNEL_PTR, 8), -EFAULT);
    expect(open((char *)0, O_RDONLY), -EFAULT);
    expect(open((char *)KERNEL_PTR, O_RDONLY), -EFAULT);
    expect(open((char *)UNMAPPED, O_RDONLY), -EFAULT);
    expect(stat("/etc/motd", (struct stat *)KERNEL_PTR), -EFAULT);
    expect(stat("/etc/motd", (struct stat *)0), -EFAULT);
    expect(stat((char *)KERNEL_PTR, &st), -EFAULT);
    expect(getcwd((char *)KERNEL_PTR, 64), -EFAULT);
    expect(getcwd((char *)UNMAPPED, 64), -EFAULT);
    expect(procinfo((struct abi_procinfo *)KERNEL_PTR, 4), -EFAULT);
    expect(meminfo((struct abi_meminfo *)UNMAPPED), -EFAULT);
    expect(wait(12345, (int *)KERNEL_PTR), -EFAULT);
    expect(syscall3(SYS_READLINK, (long)"/etc/motd", KERNEL_PTR, 16), -EINVAL); /* not a link */
    expect(syscall3(SYS_SYMLINK, KERNEL_PTR, (long)"/tmp/x", 0), -EFAULT);

    /* A buffer that ends exactly at the top of the stack: a read cannot be partial. */
    long fd = open("/etc/motd", O_RDONLY);
    expect(fd >= 3, 1);
    expect(read((int)fd, (void *)(STACK_TOP - 8), 16), -EFAULT);
    close((int)fd);

    /* Paths. */
    expect(open("", O_RDONLY), -ENOENT);
    static char longpath[2000];
    memset(longpath, 'a', sizeof(longpath) - 1);
    expect(open(longpath, O_RDONLY), -ENAMETOOLONG);
    expect(open("/etc/motd", 0x40000000), -EINVAL);
    expect(open("/etc/motd", 3), -EINVAL);
    expect(open("/nonexistent/file", O_RDONLY), -ENOENT);
    expect(open("/etc/motd/x", O_RDONLY), -ENOTDIR);
    expect(open("/etc", O_WRONLY), -EISDIR);
    expect(mkdir("/etc"), -EEXIST);
    expect(rmdir("/etc/motd"), -ENOTDIR);
    expect(unlink("/etc"), -EISDIR);
    expect(chdir("/etc/motd"), -ENOTDIR);
    expect(chdir("/nonexistent"), -ENOENT);
    expect(getcwd(buf, 1), -ERANGE);
    expect(getcwd(buf, 0), -ERANGE);

    /* Descriptor table limits: the whole table can be used and exhausted. */
    int fds[32], n = 0;
    for (;;) {
        long f = open("/etc/motd", O_RDONLY);
        if (f < 0) {
            expect(f, -EMFILE);
            break;
        }
        fds[n++] = (int)f;
        if (n == 32)
            break;
    }
    expect(n, 13); /* 16 slots, three of them are the console */
    for (int i = 0; i < n; i++)
        expect(close(fds[i]), 0);
    expect(close(fds[0]), -EBADF); /* already closed */

    /* spawn */
    char *argv_ok[] = {"hello", NULL};
    expect(spawn((char *)0, argv_ok, NULL), -EFAULT);
    expect(spawn("/bin/nonexistent", argv_ok, NULL), -ENOENT);
    expect(spawn("/bin", argv_ok, NULL), -EACCES);
    expect(spawn("/etc/motd", argv_ok, NULL), -ENOEXEC);
    expect(spawn("/bin/hello", (char *const *)KERNEL_PTR, NULL), -EFAULT);
    expect(spawn("/bin/hello", (char *const *)UNMAPPED, NULL), -EFAULT);

    char *bad_ptr_arg[] = {(char *)KERNEL_PTR, NULL};
    expect(spawn("/bin/hello", bad_ptr_arg, NULL), -EFAULT);

    char *many[ABI_MAX_ARGS + 2];
    for (int i = 0; i < ABI_MAX_ARGS + 1; i++)
        many[i] = "x";
    many[ABI_MAX_ARGS + 1] = NULL;
    expect(spawn("/bin/hello", many, NULL), -E2BIG);
    static char big_arg[ABI_ARG_MAX + 50];
    memset(big_arg, 'b', sizeof(big_arg) - 1);
    char *big_argv[] = {big_arg, NULL};
    expect(spawn("/bin/hello", big_argv, NULL), -E2BIG);

    struct abi_spawn_attr attr = {.stdout_path = "/tmp/t_sys.out", .stdout_flags = 0x7000};
    expect(spawn("/bin/hello", argv_ok, &attr), -EINVAL);
    attr.stdout_flags = O_TRUNC;
    attr.stdout_path = (char *)KERNEL_PTR;
    expect(spawn("/bin/hello", argv_ok, &attr), -EFAULT);
    expect(spawn("/bin/hello", argv_ok, (struct abi_spawn_attr *)KERNEL_PTR), -EFAULT);
    attr.stdout_path = "/etc/motd/nope"; /* cannot be opened: the spawn must fail cleanly */
    expect(spawn("/bin/hello", argv_ok, &attr), -ENOTDIR);

    /* wait */
    int status;
    long w1 = wait(1, &status); /* pid 1 (init, if running) is not our child */
    expect(w1 == -ECHILD || w1 == -ESRCH, 1);
    expect(wait(999999, &status), -ESRCH);
    expect(wait(-1, &status), -ESRCH);
    expect(wait((int)getpid(), &status), -ECHILD); /* not our own child either */

    /* sbrk misuse */
    expect(sbrk(-0x100000), -ENOMEM);            /* below the start of the heap */
    expect(sbrk(0x7FFFFFFFFFFFFFFFL), -ENOMEM);  /* absurdly large */
    expect(sbrk(-0x7FFFFFFFFFFFFFFFL), -ENOMEM); /* absurdly negative */

    if (failures)
        printf("t_sys: %d of %d checks failed\n", failures, check_no);
    return failures ? 1 : 0;
}
