#include "ulib.h"

static int cat_fd(int fd)
{
    char buf[1024];
    for (;;) {
        long n = read(fd, buf, sizeof(buf));
        if (n < 0)
            return (int)-n;
        if (n == 0)
            return 0;
        write(1, buf, (size_t)n);
    }
}

int main(int argc, char **argv)
{
    if (argc == 1)
        return cat_fd(0);
    int status = 0;
    for (int i = 1; i < argc; i++) {
        long fd = open(argv[i], O_RDONLY);
        if (fd < 0) {
            dprintf(2, "cat: %s: %s\n", argv[i], strerror((int)-fd));
            status = 1;
            continue;
        }
        int err = cat_fd((int)fd);
        if (err) {
            dprintf(2, "cat: %s: %s\n", argv[i], strerror(err));
            status = 1;
        }
        close((int)fd);
    }
    return status;
}
