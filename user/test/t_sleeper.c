#include "ulib.h"

/* Sleeps for argv[1] milliseconds, then leaves a marker file. */
int main(int argc, char **argv)
{
    sleep_ms(argc > 1 ? (unsigned long)atol(argv[1]) : 100);
    long fd = open("/tmp/t_sleeper.out", O_WRONLY | O_CREAT | O_TRUNC);
    if (fd < 0)
        return 1;
    write((int)fd, "done", 4);
    close((int)fd);
    return 0;
}
