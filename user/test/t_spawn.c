#include "ulib.h"

/* spawn/wait: arguments, exit codes, output redirection, relative paths, nesting. */

static int failures;

static void check(int ok, const char *what)
{
    if (!ok) {
        dprintf(2, "t_spawn: FAILED: %s\n", what);
        failures++;
    }
}

static int file_is(const char *path, const char *want)
{
    char buf[128];
    long fd = open(path, O_RDONLY);
    if (fd < 0)
        return 0;
    long n = read((int)fd, buf, sizeof(buf));
    close((int)fd);
    return n == (long)strlen(want) && memcmp(buf, want, (size_t)n) == 0;
}

static int run(const char *path, char *const *argv, const struct abi_spawn_attr *attr)
{
    long pid = spawn(path, argv, attr);
    if (pid < 0)
        return -1000 + (int)pid;
    int status = -1;
    long w = wait((int)pid, &status);
    if (w != pid)
        return -2000;
    if (wait((int)pid, &status) != -ESRCH) /* it has been reaped */
        return -3000;
    return status;
}

int main(int argc, char **argv)
{
    if (argc > 1 && strcmp(argv[1], "child") == 0)
        return 42;

    char *hello[] = {"t_hello", NULL};
    check(run("/tests/t_hello", hello, NULL) == 7, "exit status 7 comes back");
    check(file_is("/tmp/t_hello.out", "hello from user\n"), "child wrote its file");

    char *args[] = {"t_args", "a", "b c", "d", NULL};
    check(run("/tests/t_args", args, NULL) == 4, "argc is 4");
    check(file_is("/tmp/t_args.out", "t_args,a,b c,d"), "argv arrives intact");

    char *echo1[] = {"echo", "hi", "there", NULL};
    struct abi_spawn_attr attr = {.stdout_path = "/tmp/t_spawn.out", .stdout_flags = O_TRUNC};
    check(run("/bin/echo", echo1, &attr) == 0, "echo exits 0");
    check(file_is("/tmp/t_spawn.out", "hi there\n"), "stdout redirected to a file");
    char *echo2[] = {"echo", "again", NULL};
    attr.stdout_flags = O_APPEND;
    check(run("/bin/echo", echo2, &attr) == 0, "echo again");
    check(file_is("/tmp/t_spawn.out", "hi there\nagain\n"), "stdout appended");
    unlink("/tmp/t_spawn.out");

    check(chdir("/tests") == 0, "chdir");
    char *rel[] = {"t_args", NULL};
    check(run("t_args", rel, NULL) == 1, "relative program path resolves against the cwd");
    chdir("/");

    char *self[] = {"t_spawn", "child", NULL};
    check(run("/tests/t_spawn", self, NULL) == 42, "a child can spawn too");

    check(run("/tests/nonexistent", hello, NULL) == -1000 - ENOENT, "missing program");

    return failures ? 1 : 0;
}
