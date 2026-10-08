#include "ulib.h"

/* Console input through the kernel's line discipline (the test feeds the keys). */
static int fails(int n, long got)
{
    dprintf(2, "t_readline: check %d failed (got %ld)\n", n, got);
    return n;
}

int main(void)
{
    char buf[100];
    long n = read(0, buf, sizeof(buf));
    if (n != 4 || memcmp(buf, "abc\n", 4) != 0) /* "abx", backspace, "c" */
        return fails(1, n);
    n = read(0, buf, sizeof(buf));
    if (n != 7 || memcmp(buf, "second\n", 7) != 0)
        return fails(2, n);
    n = read(0, buf, 3); /* a line larger than the buffer is delivered in pieces */
    if (n != 3 || memcmp(buf, "lon", 3) != 0)
        return fails(3, n);
    n = read(0, buf, sizeof(buf));
    if (n != 7 || memcmp(buf, "g line\n", 7) != 0)
        return fails(4, n);
    n = read(0, buf, sizeof(buf)); /* Ctrl-C abandons the typed text */
    if (n != 1 || buf[0] != '\n')
        return fails(5, n);
    n = read(0, buf, sizeof(buf)); /* Ctrl-D at the start of a line is end of file */
    if (n != 0)
        return fails(6, n);
    return 0;
}
