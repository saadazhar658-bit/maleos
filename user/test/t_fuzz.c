#include "ulib.h"

/*
 * System call fuzzer. Fires random system calls with hostile arguments (kernel addresses,
 * unmapped pages, NULL, huge lengths, bad descriptors, paths that are too long) and checks
 * that the kernel survives, that every result is either a success or a plausible -errno, and
 * that unknown call numbers give -ENOSYS. Usage: t_fuzz [seed [iterations]]
 *
 * Calls that would end the test (exit, poweroff, reads from the console) are not generated, and
 * mutating file calls only touch /tmp/fz. The kernel self-test runs this with several seeds and
 * then checks for leaked frames, heap, descriptors and processes.
 */

#define SCRATCH_SIZE 65536
static char scratch[SCRATCH_SIZE];
static char longpath[1024];
static uint64_t rng;

static uint64_t rnd(void)
{
    rng ^= rng >> 12;
    rng ^= rng << 25;
    rng ^= rng >> 27;
    return rng * 0x2545F4914F6CDD1DUL;
}

static long pick(const long *tab, int n)
{
    return tab[rnd() % (unsigned)n];
}

enum kind { K_ANY, K_PATH, K_MPATH, K_TARGET, K_FD, K_PTR, K_LEN, K_FLAGS, K_NONE };

static long gen_ptr(void)
{
    switch (rnd() % 8) {
    case 0:
        return 0;
    case 1:
        return (long)0xFFFF800000000000UL; /* direct map */
    case 2:
        return (long)0xFFFFFFFF80000000UL; /* kernel image */
    case 3:
        return (long)(rnd() | 0x8000000000000000UL); /* random kernel half */
    case 4:
        return 0x1000; /* unmapped */
    case 5:
        return (long)&scratch[SCRATCH_SIZE - 3]; /* straddles the end of the mapping */
    case 6:
        return (long)&scratch[rnd() % SCRATCH_SIZE];
    default:
        return (long)scratch;
    }
}

/* Pointers that can never name a real path: used where the call would change the file system. */
static long gen_bad_ptr(void)
{
    static const long t[] = {0, 0x1000, (long)0xFFFF800000000000UL, (long)0xFFFFFFFF80000000UL};
    return pick(t, sizeof(t) / sizeof(t[0]));
}

static long gen_len(void)
{
    static const long t[] = {
        0,      1,      2,    7,    8,     63,       64,       255, 256,   257,
        2047,   2048,   2049, 4096, 65536, 1L << 31, 1L << 32, -1,  -4096, (long)(1UL << 63),
        100000, 1000000};
    return rnd() % 4 ? pick(t, sizeof(t) / sizeof(t[0])) : (long)(rnd() % 3000);
}

static long gen_fd(void)
{
    static const long t[] = {3, 4, 5, 6, 7, 8, 15, 16, 17, 63, 64, 1000, -1, -3, 1L << 32};
    return pick(t, sizeof(t) / sizeof(t[0]));
}

static long gen_path(int mutating)
{
    static const char *const safe[] = {
        "/etc/motd", "/",         "/tmp",           "/bin",        "..", "",
        ".",         "/dev/null", "/nonexistent/x", "/etc/motd/x", "//", "/mnt"};
    static const char *const fz[] = {"/tmp/fz",   "/tmp/fz/a", "/tmp/fz/b",
                                     "/tmp/fz/c", "/tmp/fz/d", "/tmp/fz/a/x"};
    uint64_t r = rnd() % 10;
    if (r == 0)
        return mutating ? gen_bad_ptr() : gen_ptr();
    if (r == 1)
        return (long)longpath;
    if (!mutating && r < 5)
        return (long)safe[rnd() % (sizeof(safe) / sizeof(safe[0]))];
    return (long)fz[rnd() % (sizeof(fz) / sizeof(fz[0]))];
}

/* Symlink targets stay inside /tmp/fz (or are dangling or loops): a link to a real directory
 * would let later creates escape the sandbox. */
static long gen_target(void)
{
    static const char *const t[] = {
        "x",         "a",           "b",           "loop",  "/tmp/fz/loop",
        "/tmp/fz/a", "/tmp/fz/b/y", "nonexistent", "a/b/c", "/tmp/fz"};
    uint64_t r = rnd() % 10;
    if (r == 0)
        return gen_bad_ptr();
    if (r == 1)
        return (long)longpath;
    return (long)t[rnd() % (sizeof(t) / sizeof(t[0]))];
}

static long gen_flags(void)
{
    static const long t[] = {O_RDONLY,
                             O_WRONLY,
                             O_RDWR,
                             O_CREAT | O_WRONLY,
                             O_CREAT | O_RDWR,
                             O_CREAT | O_TRUNC | O_RDWR,
                             O_APPEND | O_WRONLY,
                             3,
                             0x40000000,
                             -1,
                             O_CREAT | O_RDONLY};
    return pick(t, sizeof(t) / sizeof(t[0]));
}

static long gen(enum kind k)
{
    switch (k) {
    case K_PATH:
        return gen_path(0);
    case K_MPATH:
        return gen_path(1);
    case K_TARGET:
        return gen_target();
    case K_FD:
        return gen_fd();
    case K_PTR:
        return gen_ptr();
    case K_LEN:
        return gen_len();
    case K_FLAGS:
        return gen_flags();
    case K_NONE:
        return 0;
    default:
        return rnd() % 3 ? (long)rnd() : (long)(rnd() % 40) - 8;
    }
}

struct sig {
    enum kind k[3];
};

/* Argument kinds per system call number; calls not listed here are never generated. */
static const struct sig sigs[SYS_COUNT] = {
    [SYS_WRITE] = {{K_FD, K_PTR, K_LEN}},          [SYS_READ] = {{K_FD, K_PTR, K_LEN}},
    [SYS_OPEN] = {{K_MPATH, K_FLAGS, K_ANY}},      [SYS_CLOSE] = {{K_FD, K_NONE, K_NONE}},
    [SYS_LSEEK] = {{K_FD, K_ANY, K_ANY}},          [SYS_STAT] = {{K_PATH, K_PTR, K_NONE}},
    [SYS_LSTAT] = {{K_PATH, K_PTR, K_NONE}},       [SYS_FSTAT] = {{K_FD, K_PTR, K_NONE}},
    [SYS_READDIR] = {{K_FD, K_PTR, K_NONE}},       [SYS_MKDIR] = {{K_MPATH, K_NONE, K_NONE}},
    [SYS_RMDIR] = {{K_MPATH, K_NONE, K_NONE}},     [SYS_UNLINK] = {{K_MPATH, K_NONE, K_NONE}},
    [SYS_SYMLINK] = {{K_TARGET, K_MPATH, K_NONE}}, [SYS_READLINK] = {{K_PATH, K_PTR, K_LEN}},
    [SYS_CHDIR] = {{K_MPATH, K_NONE, K_NONE}},     [SYS_GETCWD] = {{K_PTR, K_LEN, K_NONE}},
    [SYS_GETPID] = {{K_NONE, K_NONE, K_NONE}},     [SYS_YIELD] = {{K_NONE, K_NONE, K_NONE}},
    [SYS_UPTIME_MS] = {{K_NONE, K_NONE, K_NONE}},  [SYS_SPAWN] = {{K_PATH, K_PTR, K_PTR}},
    [SYS_WAIT] = {{K_ANY, K_PTR, K_NONE}},         [SYS_SBRK] = {{K_ANY, K_NONE, K_NONE}},
    [SYS_PROCINFO] = {{K_PTR, K_LEN, K_NONE}},     [SYS_MEMINFO] = {{K_PTR, K_NONE, K_NONE}},
    [SYS_FTRUNCATE] = {{K_FD, K_LEN, K_NONE}},     [SYS_GETRANDOM] = {{K_PTR, K_LEN, K_NONE}},
};

static int usable(long nr)
{
    const struct sig *g = &sigs[nr];
    int listed = g->k[0] != K_ANY || g->k[1] != K_ANY || g->k[2] != K_ANY;
    return nr > 0 && nr < SYS_COUNT && listed && nr != SYS_POWEROFF && nr != SYS_SLEEP_MS;
}

/* Remove a file, symlink or directory tree. */
static void rmtree(const char *path, int depth)
{
    if (unlink(path) == 0 || depth > 6)
        return;
    long fd = open(path, O_RDONLY);
    if (fd >= 0) {
        char names[8][VFS_NAME_MAX];
        int n = 0;
        struct dirent de;
        while (n < 8 && readdir((int)fd, &de) > 0)
            if (strcmp(de.name, ".") && strcmp(de.name, ".."))
                strlcpy(names[n++], de.name, VFS_NAME_MAX);
        close((int)fd);
        for (int i = 0; i < n; i++) {
            char child[VFS_PATH_MAX];
            size_t len = strlcpy(child, path, sizeof(child));
            strlcpy(child + len, "/", sizeof(child) - len);
            strlcpy(child + len + 1, names[i], sizeof(child) - len - 1);
            rmtree(child, depth + 1);
        }
    }
    rmdir(path);
}

static void clean(void)
{
    for (int fd = 3; fd < 20; fd++)
        close(fd);
    chdir("/");
    for (int pass = 0; pass < 3; pass++) /* a directory can hold more than 8 entries */
        rmtree("/tmp/fz", 0);
}

int main(int argc, char **argv)
{
    uint64_t seed = 1;
    long iters = 3000;
    if (argc > 1)
        seed = (uint64_t)atol(argv[1]);
    if (argc > 2)
        iters = atol(argv[2]);
    rng = seed * 0x9E3779B97F4A7C15UL + 0x1234567;
    for (int i = 0; i < 8; i++)
        rnd();

    memset(longpath, 'x', sizeof(longpath) - 1);
    for (unsigned i = 0; i < sizeof(scratch); i++)
        scratch[i] = (char)rnd();
    mkdir("/tmp/fz");

    long ok = 0, err = 0, bad = 0;
    for (long i = 0; i < iters; i++) {
        long nr, a[3];
        if (rnd() % 16 == 0) { /* a call number that does not exist */
            nr = rnd() % 2 ? SYS_COUNT + (long)(rnd() % 5) : (long)rnd();
            if (nr >= 0 && nr < SYS_COUNT)
                nr = SYS_COUNT;
            long r = syscall3(nr, (long)rnd(), (long)rnd(), (long)rnd());
            if (r != -ENOSYS) {
                printf("fuzz: call %ld returned %ld, want -ENOSYS\n", nr, r);
                bad++;
            }
            continue;
        }
        do {
            nr = (long)(rnd() % SYS_COUNT);
        } while (!usable(nr));
        for (int s = 0; s < 3; s++)
            a[s] = gen(sigs[nr].k[s]);
        if (nr == SYS_SBRK) {
            static const long t[] = {0,  1,     4095,     4096,        100000,
                                     -1, -4096, 1L << 40, -(1L << 40), 8192};
            a[0] = pick(t, sizeof(t) / sizeof(t[0]));
        }
        long r = syscall3(nr, a[0], a[1], a[2]);
        if (r >= 0) {
            ok++;
        } else if (r >= -4095) {
            err++;
        } else {
            printf("fuzz: call %ld(%lx, %lx, %lx) returned %ld\n", nr, a[0], a[1], a[2], r);
            bad++;
        }
        if (nr == SYS_SBRK && r >= 0 && sbrk(0) < 0) {
            printf("fuzz: sbrk(0) failed after sbrk\n");
            bad++;
        }
    }

    clean();
    printf("fuzz seed=%lu iterations=%ld ok=%ld errors=%ld bad=%ld\n", (unsigned long)seed, iters,
           ok, err, bad);
    return bad ? 1 : 0;
}
