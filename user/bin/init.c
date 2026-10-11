#include "ulib.h"

/* PID 1: start the shell and start it again whenever it ends. */
int main(void)
{
    printf("\ninit: Maleos userland started (pid %ld)\n", getpid());

    /* Network services; they simply fail to start if there is nothing to bind to. */
    static char *const daemons[][3] = {
        {"/bin/httpd", "httpd", NULL},
        {"/bin/echod", "echod", NULL},
        {"/bin/echod", "echod", "udp"},
    };
    for (unsigned i = 0; i < sizeof(daemons) / sizeof(daemons[0]); i++) {
        char *dargv[] = {daemons[i][1], daemons[i][2], NULL};
        long dpid = spawn(daemons[i][0], dargv, NULL);
        if (dpid >= 0)
            printf("init: started %s (pid %ld)\n", daemons[i][1], dpid);
    }

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
