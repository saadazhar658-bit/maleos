#include "ulib.h"

/* grep pattern [file...]: print the lines that contain the pattern (plain text, no regexps).
 * Exit status 0 if something matched, 1 if not. */
static int found;

static int contains(const char *line, const char *pat)
{
    size_t n = strlen(pat);
    for (; *line; line++)
        if (strncmp(line, pat, n) == 0)
            return 1;
    return 0;
}

static int grep_fd(int fd, const char *pat, const char *label)
{
    char line[512];
    size_t len = 0;
    char buf[512];
    for (;;) {
        long n = read(fd, buf, sizeof(buf));
        if (n < 0)
            return (int)-n;
        if (n == 0)
            break;
        for (long i = 0; i < n; i++) {
            if (buf[i] == '\n' || len == sizeof(line) - 1) {
                line[len] = 0;
                if (contains(line, pat)) {
                    if (label)
                        printf("%s:", label);
                    puts(line);
                    found = 1;
                }
                len = 0;
                if (buf[i] == '\n')
                    continue;
            }
            line[len++] = buf[i];
        }
    }
    if (len) {
        line[len] = 0;
        if (contains(line, pat)) {
            if (label)
                printf("%s:", label);
            puts(line);
            found = 1;
        }
    }
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        dprintf(2, "usage: grep pattern [file...]\n");
        return 2;
    }
    if (argc == 2)
        return grep_fd(0, argv[1], NULL) ? 2 : !found;
    int status = 0;
    for (int i = 2; i < argc; i++) {
        long fd = open(argv[i], O_RDONLY);
        if (fd < 0) {
            dprintf(2, "grep: %s: %s\n", argv[i], strerror((int)-fd));
            status = 2;
            continue;
        }
        grep_fd((int)fd, argv[1], argc > 3 ? argv[i] : NULL);
        close((int)fd);
    }
    return status ? status : !found;
}
