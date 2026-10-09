#include "kernel/userland_selftest.h"

#include "arch/syscall.h"
#include "fs/vfs.h"
#include "kernel/console.h"
#include "kernel/errno.h"
#include "kernel/printk.h"
#include "kernel/process.h"
#include "kernel/random.h"
#include "kernel/sched.h"
#include "kernel/string.h"
#include "mm/heap.h"
#include "mm/mm.h"
#include "mm/pmm.h"

static int checks;

#define CHECK(cond, what)                                                                          \
    do {                                                                                           \
        checks++;                                                                                  \
        if (!(cond))                                                                               \
            kpanic("user selftest failed: %s (%s:%d)", what, __FILE__, __LINE__);                  \
    } while (0)

#define CHECK_EQ(got, want, what)                                                                  \
    do {                                                                                           \
        long long g_ = (got), w_ = (want);                                                         \
        checks++;                                                                                  \
        if (g_ != w_)                                                                              \
            kpanic("user selftest failed: %s: got %lld, want %lld (%s:%d)", what, g_, w_,          \
                   __FILE__, __LINE__);                                                            \
    } while (0)

#define SPAWN_FAILED (-100000)

/* Start a program from the kernel and wait for it. Returns its exit status. */
static long run(const char *path, int argc, const char *const *argv)
{
    int pid = process_spawn(path, argv, argc, NULL, 0, "/", 0);
    if (pid < 0)
        return SPAWN_FAILED + pid;
    int status = -1;
    CHECK_EQ(process_wait(pid, 0, &status), pid, "wait returns the pid");
    return status;
}

static long run0(const char *path)
{
    const char *argv[] = {path};
    return run(path, 1, argv);
}

static long run1(const char *path, const char *arg)
{
    const char *argv[] = {path, arg};
    return run(path, 2, argv);
}

static bool file_is(const char *path, const char *want)
{
    char buf[128];
    int64_t n = vfs_read_file(path, buf, sizeof(buf));
    return n == (int64_t)strlen(want) && memcmp(buf, want, (size_t)n) == 0;
}

static uint64_t heap_used(void)
{
    struct heap_stats hs;
    heap_get_stats(&hs);
    return hs.used_bytes;
}

/* ---------- programs ---------- */

static void test_basic_programs(void)
{
    CHECK_EQ(run0("/tests/t_hello"), 7, "t_hello exit status");
    CHECK(file_is("/tmp/t_hello.out", "hello from user\n"), "t_hello wrote its file");

    const char *argv[] = {"t_args", "a", "b c", "d"};
    CHECK_EQ(run("/tests/t_args", 4, argv), 4, "t_args exit status is argc");
    CHECK(file_is("/tmp/t_args.out", "t_args,a,b c,d"), "arguments reach the program intact");

    CHECK_EQ(run0("/tests/t_brk"), 0, "heap management (sbrk)");
    CHECK_EQ(run0("/tests/t_files"), 0, "file system calls from user space");
    CHECK_EQ(run0("/tests/t_spawn"), 0, "spawn, wait and redirection");
    CHECK_EQ(run0("/tests/t_sys"), 0, "invalid system call arguments are rejected");
    CHECK_EQ(run1("/tests/t_iso", "1"), 0, "isolation test, single instance");
    CHECK_EQ(process_count(), 0, "no process left behind");
}

static void test_faults(void)
{
    static const struct {
        const char *kind;
        int status;
    } cases[] = {
        {"null", EXIT_SIGSEGV},      {"nullread", EXIT_SIGSEGV}, {"kread", EXIT_SIGSEGV},
        {"kwrite", EXIT_SIGSEGV},    {"kjump", EXIT_SIGSEGV},    {"text", EXIT_SIGSEGV},
        {"stackexec", EXIT_SIGSEGV}, {"stack", EXIT_SIGSEGV},    {"cli", EXIT_SIGSEGV},
        {"hlt", EXIT_SIGSEGV},       {"io", EXIT_SIGSEGV},       {"cr3", EXIT_SIGSEGV},
        {"msr", EXIT_SIGSEGV},       {"ud", EXIT_SIGILL},        {"div", EXIT_SIGFPE},
        {"int80", EXIT_SIGSEGV},     {"int30", EXIT_SIGSEGV},    {"segment", EXIT_SIGSEGV},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        long st = run1("/tests/t_fault", cases[i].kind);
        if (st != cases[i].status)
            kpanic("user selftest failed: t_fault %s ended with status %ld, want %d", cases[i].kind,
                   st, cases[i].status);
        checks++;
        CHECK_EQ(process_count(), 0, "a killed process is fully reaped");
    }
    CHECK_EQ(run1("/tests/t_fault", "nothing"), 0, "t_fault with an unknown mode just returns");
}

static void test_concurrency(void)
{
    CHECK_EQ(run0("/tests/t_par"), 0, "eight isolated processes at once");

    /* 20 more from the kernel side, all started before any is waited for. */
    int pids[20];
    for (int i = 0; i < 20; i++) {
        char id[4] = {(char)('a' + i), 0};
        const char *argv[] = {"t_iso", id};
        pids[i] = process_spawn("/tests/t_iso", argv, 2, NULL, 0, "/", 0);
        CHECK(pids[i] > 0, "spawn");
    }
    int bad = 0;
    for (int i = 0; i < 20; i++) {
        int st = -1;
        if (process_wait(pids[i], 0, &st) != pids[i] || st != 0)
            bad++;
    }
    CHECK_EQ(bad, 0, "20 concurrent processes all finish cleanly");

    /* The process table is finite: surplus spawns fail, nothing breaks. */
    int started = 0, refused = 0, started_pids[PROC_MAX + 8];
    for (int i = 0; i < PROC_MAX + 8; i++) {
        const char *argv[] = {"t_sleeper", "150"};
        int pid = process_spawn("/tests/t_sleeper", argv, 2, NULL, 0, "/", 0);
        if (pid > 0)
            started_pids[started++] = pid;
        else if (pid == -ENOMEM)
            refused++;
    }
    CHECK_EQ(started, PROC_MAX, "the table fills up to PROC_MAX");
    CHECK_EQ(refused, 8, "spawns beyond the limit fail with ENOMEM");
    for (int i = 0; i < started; i++) {
        int st = -1;
        CHECK(process_wait(started_pids[i], 0, &st) == started_pids[i] && st == 0, "sleeper ends");
    }
    CHECK_EQ(process_count(), 0, "table empty again");
}

static void test_orphans(void)
{
    vfs_unlink("/tmp/t_sleeper.out");
    CHECK_EQ(run0("/tests/t_orphan"), 0, "t_orphan exits at once");
    /* Its child is still sleeping; the reaper must collect it when it ends. */
    int waited = 0;
    while (process_count() > 0 && waited < 3000) {
        thread_sleep_ms(50);
        waited += 50;
    }
    CHECK_EQ(process_count(), 0, "the orphan was reaped");
    CHECK(file_is("/tmp/t_sleeper.out", "done"), "the orphan finished its work");
}

static void test_console_input(void)
{
    static const char keys[] = "abx\bc\n"
                               "second\n"
                               "long line\n"
                               "junk\x03"
                               "\x04";
    console_flush_input();
    console_feed(keys, sizeof(keys) - 1);
    CHECK_EQ(run0("/tests/t_readline"), 0,
             "line discipline: echo, backspace, ^C, ^D, partial reads");
}

/* ---------- the ELF loader ---------- */

/* ---------- hardening ---------- */

static uint64_t parse_hex(const char **pp)
{
    const char *p = *pp;
    uint64_t v = 0;
    while ((*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'f'))
        v = v * 16 + (uint64_t)(*p <= '9' ? *p - '0' : *p - 'a' + 10), p++;
    *pp = p;
    return v;
}

#define ASLR_RUNS 24

static void test_hardening(void)
{
    /* Canary: random, with a zero low byte. */
    CHECK((__stack_chk_guard & 0xff) == 0, "the canary has a zero byte");
    CHECK(__stack_chk_guard != 0x595e9fbd94fda766UL, "the canary was randomised at boot");

    uint64_t a = random_u64(), b = random_u64(), c = random_u64();
    CHECK(a != b && b != c && a != c, "random_u64 does not repeat");

    /* A user-space buffer overflow is caught by the stack protector. */
    CHECK_EQ(run0("/tests/t_smash"), 134, "user stack protector kills the overflowing program");

    /* ASLR: the stack and the heap start at different addresses across runs. */
    uint64_t stacks[ASLR_RUNS], heaps[ASLR_RUNS];
    for (int i = 0; i < ASLR_RUNS; i++) {
        const char *argv[] = {"t_aslr"};
        int pid = process_spawn("/tests/t_aslr", argv, 1, "/tmp/aslr.out", O_TRUNC, "/", 0);
        CHECK(pid > 0, "t_aslr starts");
        int st = -1;
        CHECK_EQ(process_wait(pid, 0, &st), pid, "t_aslr is waited for");
        CHECK_EQ(st, 0, "t_aslr exit status");
        char buf[64] = {0};
        CHECK(vfs_read_file("/tmp/aslr.out", buf, sizeof(buf) - 1) > 3, "t_aslr output");
        const char *q = buf;
        stacks[i] = parse_hex(&q);
        CHECK(*q == ' ', "t_aslr output format");
        q++;
        heaps[i] = parse_hex(&q);
        CHECK(stacks[i] < USER_STACK_TOP && stacks[i] > USER_STACK_TOP - 0x2000000UL,
              "stack address in range");
        CHECK(heaps[i] >= 0x400000 && heaps[i] < 0x400000 + 0x2000000UL, "heap address in range");
        CHECK((heaps[i] & (PAGE_SIZE - 1)) == 0, "heap start is page aligned");
    }
    vfs_unlink("/tmp/aslr.out");
    int ds = 0, dh = 0;
    for (int i = 0; i < ASLR_RUNS; i++) {
        bool ns = true, nh = true;
        for (int j = 0; j < i; j++) {
            ns &= stacks[j] != stacks[i];
            nh &= heaps[j] != heaps[i];
        }
        ds += ns;
        dh += nh;
    }
    CHECK(ds >= 12, "the stack address varies between runs");
    CHECK(dh >= 12, "the heap address varies between runs");
}

static void test_fuzz(void)
{
    static const char *const seeds[] = {"1", "2", "3", "4", "5", "6", "7", "8"};
    for (size_t i = 0; i < sizeof(seeds) / sizeof(seeds[0]); i++) {
        const char *argv[] = {"t_fuzz", seeds[i], "2500"};
        CHECK_EQ(run("/tests/t_fuzz", 3, argv), 0, "syscall fuzzer finds no bad result");
        CHECK_EQ(process_count(), 0, "fuzzer process is gone");
        CHECK_EQ(vfs_open_file_count(), 0, "fuzzer left no descriptors behind");
    }
}

static uint16_t rd16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

static void wr(uint8_t *p, uint64_t v, int bytes)
{
    for (int i = 0; i < bytes; i++)
        p[i] = (uint8_t)(v >> (8 * i));
}

static int try_load(const uint8_t *img, size_t size)
{
    struct process *p;
    const char *argv[] = {"fuzz"};
    int rc = process_load(img, size, "fuzz", argv, 1, "/", 0, &p);
    if (rc == 0)
        process_discard(p);
    return rc;
}

#define PH(i) (orig + phoff + 56 * (i))

static void test_elf_loader(void)
{
    struct stat st;
    CHECK_EQ(vfs_stat("/bin/hello", &st), 0, "stat /bin/hello");
    size_t size = (size_t)st.size;
    uint8_t *orig = kmalloc(size);
    uint8_t *img = kmalloc(size);
    CHECK(orig && img, "image buffers");
    CHECK_EQ(vfs_read_file("/bin/hello", orig, size), (long long)size, "read the image");

    uint64_t phoff = 64;
    CHECK(rd16(orig + 56) >= 2, "hello has at least two segments");
    uint64_t frames = pmm_free_frame_count();
    uint64_t heap = heap_used();

    memcpy(img, orig, size);
    CHECK_EQ(try_load(img, size), 0, "the unmodified image loads");

#define MUTATE(desc, stmt, want)                                                                   \
    do {                                                                                           \
        memcpy(img, orig, size);                                                                   \
        uint8_t *ph0 = img + phoff, *ph1 = img + phoff + 56;                                       \
        (void)ph0;                                                                                 \
        (void)ph1;                                                                                 \
        stmt;                                                                                      \
        CHECK_EQ(try_load(img, size), (want), desc);                                               \
    } while (0)

    MUTATE("bad magic", img[0] ^= 1, -ENOEXEC);
    MUTATE("32-bit class", img[4] = 1, -ENOEXEC);
    MUTATE("big endian", img[5] = 2, -ENOEXEC);
    MUTATE("shared object", wr(img + 16, 3, 2), -ENOEXEC);
    MUTATE("wrong machine", wr(img + 18, 3, 2), -ENOEXEC);
    MUTATE("no program headers", wr(img + 56, 0, 2), -ENOEXEC);
    MUTATE("17 program headers", wr(img + 56, 17, 2), -ENOEXEC);
    MUTATE("65535 program headers", wr(img + 56, 0xFFFF, 2), -ENOEXEC);
    MUTATE("wrong phentsize", wr(img + 54, 32, 2), -ENOEXEC);
    MUTATE("phoff at the end of the file", wr(img + 32, size, 8), -ENOEXEC);
    MUTATE("phoff overflows", wr(img + 32, 0xFFFFFFFFFFFFFFF0ULL, 8), -ENOEXEC);
    MUTATE("segment in kernel space", wr(ph0 + 16, 0xFFFF800000000000ULL, 8), -ENOEXEC);
    MUTATE("segment at address 0", wr(ph0 + 16, 0, 8), -ENOEXEC);
    MUTATE("segment over the stack", wr(ph0 + 16, 0x00007FFFFFFFF000ULL, 8), -ENOEXEC);
    MUTATE("segment end wraps around", wr(ph0 + 16, 0xFFFFFFFFFFFFF000ULL, 8), -ENOEXEC);
    MUTATE("filesz > memsz", wr(ph0 + 32, rd16(ph0 + 40) + 0x10000ULL, 8), -ENOEXEC);
    MUTATE("offset at end of file", wr(ph0 + 8, size, 8), -ENOEXEC);
    MUTATE("offset overflows", wr(ph0 + 8, 0xFFFFFFFFFFFFFFF0ULL, 8), -ENOEXEC);
    MUTATE("data past end of file", wr(ph0 + 32, size, 8); wr(ph0 + 40, size, 8), -ENOEXEC);
    MUTATE("writable and executable", wr(ph0 + 4, 7, 4), -ENOEXEC);
    MUTATE("memsz of one terabyte", wr(ph0 + 40, 1ULL << 40, 8), -ENOEXEC);
    MUTATE("memsz of 2^64-1", wr(ph0 + 40, ~0ULL, 8), -ENOEXEC);
    MUTATE("entry at 0", wr(img + 24, 0, 8), -ENOEXEC);
    MUTATE("entry in the kernel", wr(img + 24, 0xFFFFFFFF80100000ULL, 8), -ENOEXEC);
    MUTATE("entry in a non-executable segment", wr(img + 24, 0x402000, 8), -ENOEXEC);
    MUTATE("overlapping segments", memcpy(ph1 + 16, ph0 + 16, 8), -ENOEXEC);
    MUTATE("memory quota exceeded", wr(ph0 + 40, 12u << 20, 8); wr(ph1 + 40, 12u << 20, 8);
           wr(ph0 + 32, 0, 8), -ENOMEM);

    /* Truncations: anything that cuts into a loaded segment must be rejected. */
    uint64_t loaded_end = 0;
    for (int i = 0; i < rd16(orig + 56); i++) {
        const uint8_t *ph = PH(i);
        uint64_t end = 0, filesz = 0;
        for (int b = 7; b >= 0; b--) {
            end = (end << 8) | ph[8 + b];
            filesz = (filesz << 8) | ph[32 + b];
        }
        if (filesz && end + filesz > loaded_end)
            loaded_end = end + filesz;
    }
    CHECK(loaded_end > 0x400 && loaded_end < size, "segment extent found");
    static const size_t cuts[] = {0, 1, 16, 63, 64, 100, 200, 0x400, 0x800, 0x1000, 0x1800};
    for (size_t i = 0; i < sizeof(cuts) / sizeof(cuts[0]); i++) {
        if (cuts[i] >= loaded_end)
            continue;
        memcpy(img, orig, size);
        CHECK(try_load(img, cuts[i]) < 0, "truncated image is rejected");
    }
    memcpy(img, orig, size);
    CHECK(try_load(img, loaded_end - 1) < 0, "one byte short of the last segment is rejected");
    CHECK_EQ(try_load(img, loaded_end), 0, "exactly the loaded extent is enough");
    for (size_t cut = size > 4096 ? size - 4096 : 0; cut < size; cut += 61) {
        memcpy(img, orig, size);
        try_load(img, cut); /* must simply not crash */
        checks++;
    }

    /* Random damage to the headers: whatever happens, nothing may crash or leak. */
    uint32_t seed = 12345;
    int accepted = 0;
    for (int i = 0; i < 600; i++) {
        memcpy(img, orig, size);
        int flips = 1 + (int)(seed >> 28) % 4;
        for (int f = 0; f < flips; f++) {
            seed = seed * 1664525u + 1013904223u;
            size_t at = (seed >> 8) % 256;
            seed = seed * 1664525u + 1013904223u;
            img[at] ^= (uint8_t)(1u << ((seed >> 8) % 8));
        }
        seed = seed * 1664525u + 1013904223u;
        accepted += try_load(img, size) == 0;
    }
    checks++;
    printk("  ELF fuzz: 600 damaged headers, %d still loadable\n", accepted);

    CHECK_EQ(pmm_free_frame_count(), frames, "failed loads return every frame");
    CHECK_EQ(heap_used(), heap, "failed loads leak no heap memory");
    kfree(orig);
    kfree(img);

    /* Spawn-level errors. */
    CHECK_EQ(process_spawn("/nonexistent", NULL, 0, NULL, 0, "/", 0), -ENOENT,
             "spawn: missing file");
    CHECK_EQ(process_spawn("/bin", NULL, 0, NULL, 0, "/", 0), -EACCES, "spawn: a directory");
    CHECK_EQ(process_spawn("/etc/motd", NULL, 0, NULL, 0, "/", 0), -ENOEXEC, "spawn: a text file");
    int fd = vfs_open("/tmp/empty_exec", O_WRONLY | O_CREAT);
    vfs_close(fd);
    CHECK_EQ(process_spawn("/tmp/empty_exec", NULL, 0, NULL, 0, "/", 0), -ENOEXEC,
             "spawn: empty file");
    vfs_unlink("/tmp/empty_exec");
    const char *argv[17] = {0};
    for (int i = 0; i < 17; i++)
        argv[i] = "x";
    CHECK_EQ(process_spawn("/bin/hello", argv, 17, NULL, 0, "/", 0), -E2BIG, "spawn: 17 arguments");
    CHECK_EQ(pmm_free_frame_count(), frames, "failed spawns return every frame");
}

int userland_selftest(void)
{
    checks = 0;

    /* Warm-up: the first run allocates things that stay allocated (page tables, etc.). */
    run0("/tests/t_hello");
    vfs_unlink("/tmp/t_hello.out");
    {
        /* The fuzzer makes files of up to 1 MB; that raises the heap's high-water mark once. */
        int fd = vfs_open("/tmp/warmup", O_RDWR | O_CREAT);
        if (fd >= 0) {
            vfs_ftruncate(fd, 1000000);
            vfs_close(fd);
            vfs_unlink("/tmp/warmup");
        }
        const char *fa[] = {"t_fuzz", "99", "2500"};
        run("/tests/t_fuzz", 3, fa);
    }
    uint64_t frames = pmm_free_frame_count();
    uint64_t heap = heap_used();

    test_basic_programs();
    test_faults();
    test_concurrency();
    test_orphans();
    test_console_input();
    test_elf_loader();
    test_hardening();
    test_fuzz();

    /* The programs leave marker files behind; remove them so the leak check is exact. */
    vfs_unlink("/tmp/t_hello.out");
    vfs_unlink("/tmp/t_args.out");
    vfs_unlink("/tmp/t_sleeper.out");

    CHECK_EQ(process_count(), 0, "no processes left");
    CHECK_EQ(vfs_open_file_count(), 0, "no descriptors leaked by user programs");
    CHECK_EQ(pmm_free_frame_count(), frames, "no physical frames leaked");
    CHECK_EQ(heap_used(), heap, "no kernel heap leaked");

    printk("  SMEP %s, SMAP %s\n", syscall_smep_enabled() ? "on" : "not available",
           syscall_smap_enabled() ? "on" : "not available");
    return checks;
}
