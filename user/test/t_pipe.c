#include "ulib.h"

/*
 * Pipes, dup/dup2 and descriptor inheritance.
 *   t_pipe                  runs the checks
 *   t_pipe produce N        child: writes N bytes (pattern i % 251) to stdout
 *   t_pipe consume N        child: reads stdin until EOF, exit 0 when it got exactly N
 *                           bytes of the pattern
 *   t_pipe relay            child: copies stdin to stdout until EOF
 */
static int failures;

static void check(long got, long want, const char *what)
{
    if (got != want) {
        dprintf(2, "t_pipe: %s: got %ld, want %ld\n", what, got, want);
        failures++;
    }
}

static int produce(long n)
{
    char buf[700];
    long sent = 0;
    while (sent < n) {
        long chunk = n - sent < (long)sizeof(buf) ? n - sent : (long)sizeof(buf);
        for (long i = 0; i < chunk; i++)
            buf[i] = (char)((sent + i) % 251);
        long w = write(1, buf, (size_t)chunk);
        if (w != chunk)
            return 1;
        sent += chunk;
    }
    return 0;
}

static int consume(long n)
{
    char buf[1000];
    long got = 0;
    for (;;) {
        long r = read(0, buf, sizeof(buf));
        if (r < 0)
            return 2;
        if (r == 0)
            break;
        for (long i = 0; i < r; i++)
            if (buf[i] != (char)((got + i) % 251))
                return 3;
        got += r;
    }
    return got == n ? 0 : 4;
}

static int relay(void)
{
    char buf[512];
    for (;;) {
        long r = read(0, buf, sizeof(buf));
        if (r <= 0)
            return r < 0;
        if (write(1, buf, (size_t)r) != r)
            return 1;
    }
}

static long run_child(const char *mode, const char *arg, struct abi_spawn_attr *attr)
{
    char *argv[] = {"t_pipe", (char *)mode, (char *)arg, NULL};
    return spawn("/tests/t_pipe", argv, attr);
}

static long drain(int fd)
{
    char buf[900];
    long total = 0;
    for (;;) {
        long r = read(fd, buf, sizeof(buf));
        if (r <= 0)
            return r < 0 ? r : total;
        total += r;
    }
}

int main(int argc, char **argv)
{
    if (argc >= 2 && strcmp(argv[1], "produce") == 0)
        return produce(atol(argv[2]));
    if (argc >= 2 && strcmp(argv[1], "consume") == 0)
        return consume(atol(argv[2]));
    if (argc >= 2 && strcmp(argv[1], "relay") == 0)
        return relay();

    struct abi_meminfo mi0, mi1;
    meminfo(&mi0);

    /* basics inside one process */
    int p[2];
    check(pipe(p), 0, "pipe");
    check(p[0] >= 3 && p[1] > p[0], 1, "descriptor numbers");
    check(write(p[1], "hello", 5), 5, "write");
    char buf[16];
    check(read(p[0], buf, sizeof(buf)), 5, "read");
    check(memcmp(buf, "hello", 5), 0, "data");
    check(write(p[0], "x", 1), -EBADF, "write to the read end");
    check(read(p[1], buf, 1), -EBADF, "read from the write end");

    /* end of file: no writers left */
    check(write(p[1], "ab", 2), 2, "write before close");
    check(close(p[1]), 0, "close write end");
    check(read(p[0], buf, sizeof(buf)), 2, "buffered data after the writer closed");
    check(read(p[0], buf, sizeof(buf)), 0, "end of file");
    check(close(p[0]), 0, "close read end");

    /* EPIPE: no readers left */
    check(pipe(p), 0, "second pipe");
    close(p[0]);
    check(write(p[1], "x", 1), -EPIPE, "write without readers");
    close(p[1]);

    /* dup keeps an end alive */
    check(pipe(p), 0, "third pipe");
    long d = dup(p[1]);
    check(d >= 3 && d != p[1], 1, "dup");
    close(p[1]);
    check(write((int)d, "z", 1), 1, "write through the duplicate");
    check(read(p[0], buf, 1), 1, "read it back");
    close((int)d);
    check(read(p[0], buf, 1), 0, "eof after the last duplicate closed");
    close(p[0]);

    /* dup2 */
    check(pipe(p), 0, "fourth pipe");
    check(dup2(p[1], 10), 10, "dup2 to a chosen number");
    check(dup2(p[1], p[1]), p[1], "dup2 onto itself");
    check(dup2(99, 10), -EBADF, "dup2 from a bad descriptor");
    check(dup2(p[1], 99), -EBADF, "dup2 to a bad descriptor");
    check(dup(99), -EBADF, "dup of a bad descriptor");
    /* duplicated files share one offset and stay open until the last descriptor closes */
    long f = open("/etc/motd", O_RDONLY);
    long f2 = dup((int)f);
    check(f2 >= 3 && f2 != f, 1, "files can be duplicated");
    char c1 = 0, c2 = 0, c3 = 0;
    check(read((int)f, &c1, 1), 1, "first byte through the original");
    check(read((int)f2, &c2, 1), 1, "second byte through the duplicate");
    check(lseek((int)f, 0, 0), 0, "rewind through the original");
    check(read((int)f2, &c3, 1), 1, "the duplicate sees the rewind");
    check(c3 == c1 && c2 != 0, 1, "shared offset");
    close((int)f);
    check(read((int)f2, &c3, 1), 1, "still readable after the original closed");
    close((int)f2);
    check(read((int)f2, &c3, 1), -EBADF, "closed for good");
    long sk = socket(SOCK_DGRAM);
    long sk2 = dup((int)sk);
    check(sk2 >= 3, 1, "sockets can be duplicated");
    close((int)sk);
    check(sockctl((int)sk2, SOCKCTL_GETNAME, 0) >= 0, 1, "duplicate keeps the socket alive");
    close((int)sk2);
    check(dup2((int)sk2, 12), -EBADF, "dup2 from a closed descriptor");
    close(10);
    close(p[0]);
    close(p[1]);

    /* error paths */
    volatile long badptr = 8;
    check(pipe((int *)badptr), -EFAULT, "bad pointer");
    int many[16];
    int made = 0;
    for (int i = 0; i < 16; i++) {
        int q[2];
        if (pipe(q) != 0)
            break;
        many[made++] = q[0];
        many[made++] = q[1];
        if (made >= 14)
            break;
    }
    int qq[2];
    check(pipe(qq), -EMFILE, "descriptor table full");
    for (int i = 0; i < made; i++)
        close(many[i]);

    /* a child writes 100000 bytes through a 4 KiB pipe: both sides must block and resume */
    check(pipe(p), 0, "pipe for a child");
    struct abi_spawn_attr attr = {.flags = SPAWN_STDOUT_FD, .stdout_fd = p[1]};
    long pid = run_child("produce", "100000", &attr);
    check(pid > 0, 1, "spawn producer");
    close(p[1]); /* the child holds the only write end now */
    check(drain(p[0]), 100000, "bytes received from the producer");
    int status = -1;
    wait((int)pid, &status);
    check(status, 0, "producer exit status");
    close(p[0]);

    /* the other direction: we write, the child verifies */
    check(pipe(p), 0, "pipe to a child");
    struct abi_spawn_attr in = {.flags = SPAWN_STDIN_FD, .stdin_fd = p[0]};
    pid = run_child("consume", "50000", &in);
    check(pid > 0, 1, "spawn consumer");
    close(p[0]);
    long sent = 0;
    while (sent < 50000) {
        char chunk[1100];
        long n = 50000 - sent < (long)sizeof(chunk) ? 50000 - sent : (long)sizeof(chunk);
        for (long i = 0; i < n; i++)
            chunk[i] = (char)((sent + i) % 251);
        if (write(p[1], chunk, (size_t)n) != n)
            break;
        sent += n;
    }
    check(sent, 50000, "bytes sent to the consumer");
    close(p[1]);
    status = -1;
    wait((int)pid, &status);
    check(status, 0, "consumer verified the data");

    /* producer | relay | consumer: a three-stage pipeline */
    int a[2], b[2];
    check(pipe(a), 0, "pipeline pipe 1");
    check(pipe(b), 0, "pipeline pipe 2");
    struct abi_spawn_attr s1 = {.flags = SPAWN_STDOUT_FD, .stdout_fd = a[1]};
    struct abi_spawn_attr s2 = {
        .flags = SPAWN_STDIN_FD | SPAWN_STDOUT_FD, .stdin_fd = a[0], .stdout_fd = b[1]};
    struct abi_spawn_attr s3 = {.flags = SPAWN_STDIN_FD, .stdin_fd = b[0]};
    long p1 = run_child("produce", "30000", &s1);
    long p2 = run_child("relay", "", &s2);
    long p3 = run_child("consume", "30000", &s3);
    check(p1 > 0 && p2 > 0 && p3 > 0, 1, "spawn the pipeline");
    close(a[0]);
    close(a[1]);
    close(b[0]);
    close(b[1]);
    int s[3] = {-1, -1, -1};
    wait((int)p1, &s[0]);
    wait((int)p2, &s[1]);
    wait((int)p3, &s[2]);
    check(s[0] + s[1] + s[2], 0, "all three stages succeeded");

    /* a child that exits while holding both ends must not leak the pipe */
    check(pipe(p), 0, "pipe to be abandoned");
    struct abi_spawn_attr both = {
        .flags = SPAWN_STDIN_FD | SPAWN_STDOUT_FD, .stdin_fd = p[0], .stdout_fd = p[1]};
    pid = run_child("produce", "10", &both);
    close(p[0]);
    close(p[1]);
    status = -1;
    wait((int)pid, &status);
    check(status, 0, "child writing to a pipe it also reads");

    /* a child reads a file through a redirected stdin and through an inherited descriptor */
    struct stat mst;
    stat("/etc/motd", &mst);
    check(pipe(p), 0, "pipe for file output");
    struct abi_spawn_attr from_file = {
        .stdin_path = "/etc/motd", .flags = SPAWN_STDOUT_FD, .stdout_fd = p[1]};
    pid = run_child("relay", "", &from_file);
    close(p[1]);
    check(drain(p[0]), (long)mst.size, "stdin_path delivered the whole file");
    wait((int)pid, &status);
    close(p[0]);
    struct abi_spawn_attr no_file = {.stdin_path = "/nonexistent"};
    check(run_child("relay", "", &no_file), -ENOENT, "missing stdin_path");
    struct abi_spawn_attr both_in = {
        .stdin_path = "/etc/motd", .flags = SPAWN_STDIN_FD, .stdin_fd = 1};
    check(run_child("relay", "", &both_in), -EINVAL, "stdin path and descriptor together");
    long file_fd = open("/etc/motd", O_RDONLY);
    check(pipe(p), 0, "pipe for inherited file");
    struct abi_spawn_attr inherit_file = {
        .flags = SPAWN_STDIN_FD | SPAWN_STDOUT_FD, .stdin_fd = (int)file_fd, .stdout_fd = p[1]};
    pid = run_child("relay", "", &inherit_file);
    close(p[1]);
    close((int)file_fd);
    check(drain(p[0]), (long)mst.size, "an inherited file descriptor works in the child");
    wait((int)pid, &status);
    close(p[0]);

    /* spawn argument errors */
    struct abi_spawn_attr bad = {.flags = SPAWN_STDOUT_FD, .stdout_fd = 77};
    check(run_child("produce", "1", &bad), -EBADF, "spawn with a bad descriptor");
    struct abi_spawn_attr bad2 = {.flags = 64};
    check(run_child("produce", "1", &bad2), -EINVAL, "spawn with unknown flags");
    struct abi_spawn_attr bad3 = {
        .stdout_path = "/tmp/x", .flags = SPAWN_STDOUT_FD, .stdout_fd = 1};
    check(run_child("produce", "1", &bad3), -EINVAL, "path and descriptor together");

    meminfo(&mi1);
    check((long)mi1.free_frames >= (long)mi0.free_frames - 2, 1, "no frames leaked");
    return failures;
}
