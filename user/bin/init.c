#include "ulib.h"

/* PID 1: start the shell and start it again whenever it ends. */
int main(void)
{
    printf("\ninit: Maleos userland started (pid %ld)\n", getpid());

    int quick_exits = 0;
    for (;;) {
        unsigned long started = uptime_ms();
        char *argv[] = {"sh", NULL};
        long pid = spawn("/bin/sh", argv, NULL);
        if (pid < 0) {
            printf("init: cannot start /bin/sh: %s\n", strerror((int)-pid));
            poweroff();
        }
        int status = 0;
        wait((int)pid, &status);
        printf("init: shell exited with status %d\n", status);

        quick_exits = (uptime_ms() - started < 1000) ? quick_exits + 1 : 0;
        if (quick_exits >= 3) {
            printf("init: shell keeps failing, powering off\n");
            poweroff();
        }
    }
}
