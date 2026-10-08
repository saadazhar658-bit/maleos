#include "ulib.h"

/* Starts a child and exits without waiting for it: the kernel must clean up after the child. */
int main(void)
{
    char *argv[] = {"t_sleeper", "300", NULL};
    return spawn("/tests/t_sleeper", argv, NULL) > 0 ? 0 : 1;
}
