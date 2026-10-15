#include "ulib.h"

/* wc [file...]: lines, words and bytes (standard input when no file is given). */
static int count(int fd, const char *name)
{
    char buf[1024];
    unsigned long lines = 0, words = 0, bytes = 0;
    int in_word = 0;
    for (;;) {
        long n = read(fd, buf, sizeof(buf));
        if (n < 0)
            return (int)-n;
        if (n == 0)
            break;
        for (long i = 0; i < n; i++) {
            char c = buf[i];
            if (c == '\n')
                lines++;
            if (c == ' ' || c == '\n' || c == '\t') {
                in_word = 0;
            } else if (!in_word) {
                in_word = 1;
                words++;
            }
        }
        bytes += (unsigned long)n;
    }
    printf("%lu %lu %lu%s%s\n", lines, words, bytes, name ? " " : "", name ? name : "");
    return 0;
}

int main(int argc, char **argv)
{
    if (argc == 1)
        return count(0, NULL);
    int status = 0;
    for (int i = 1; i < argc; i++) {
        long fd = open(argv[i], O_RDONLY);
        if (fd < 0) {
            dprintf(2, "wc: %s: %s\n", argv[i], strerror((int)-fd));
            status = 1;
            continue;
        }
        int err = count((int)fd, argv[i]);
        if (err) {
            dprintf(2, "wc: %s: %s\n", argv[i], strerror(err));
            status = 1;
        }
        close((int)fd);
    }
    return status;
}
