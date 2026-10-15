#include "ulib.h"

/* head [-n N] [file]: the first N lines (default 10) of a file or standard input. */
int main(int argc, char **argv)
{
    long limit = 10;
    int ai = 1;
    if (argc > 2 && strcmp(argv[1], "-n") == 0) {
        limit = atol(argv[2]);
        ai = 3;
    }
    int fd = 0;
    if (ai < argc) {
        long f = open(argv[ai], O_RDONLY);
        if (f < 0) {
            dprintf(2, "head: %s: %s\n", argv[ai], strerror((int)-f));
            return 1;
        }
        fd = (int)f;
    }
    char buf[512];
    long lines = 0;
    while (lines < limit) {
        long n = read(fd, buf, sizeof(buf));
        if (n <= 0)
            break;
        long end = 0;
        while (end < n && lines < limit) {
            if (buf[end++] == '\n')
                lines++;
        }
        write(1, buf, (size_t)end);
    }
    return 0;
}
