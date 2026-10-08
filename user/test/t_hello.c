#include "ulib.h"

/* Writes a marker file and exits with status 7: checks exit codes, file I/O and the console. */
int main(void)
{
    long fd = open("/tmp/t_hello.out", O_WRONLY | O_CREAT | O_TRUNC);
    if (fd < 0)
        return 1;
    write((int)fd, "hello from user\n", 16);
    close((int)fd);
    printf("t_hello: running as pid %ld\n", getpid());
    return 7;
}
