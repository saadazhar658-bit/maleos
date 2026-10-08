#include "ulib.h"

/* Records argv in /tmp/t_args.out (comma separated) and exits with argc. */
int main(int argc, char **argv)
{
    char line[256];
    size_t n = 0;
    for (int i = 0; i < argc; i++) {
        if (i)
            line[n++] = ',';
        n += strlcpy(line + n, argv[i], sizeof(line) - n);
    }
    long fd = open("/tmp/t_args.out", O_WRONLY | O_CREAT | O_TRUNC);
    if (fd < 0)
        return 100;
    write((int)fd, line, n);
    close((int)fd);
    return argc;
}
