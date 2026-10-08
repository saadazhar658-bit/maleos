#include "ulib.h"

/* Start eight isolation tests at the same time and wait for all of them. */
int main(void)
{
    int pids[8];
    for (int i = 0; i < 8; i++) {
        char id[8];
        id[0] = (char)('1' + i);
        id[1] = 0;
        char *argv[] = {"t_iso", id, NULL};
        long pid = spawn("/tests/t_iso", argv, NULL);
        if (pid < 0)
            return 1;
        pids[i] = (int)pid;
    }
    int bad = 0;
    for (int i = 0; i < 8; i++) {
        int status = -1;
        if (wait(pids[i], &status) != pids[i] || status != 0) {
            dprintf(2, "t_par: child %d ended with status %d\n", i, status);
            bad++;
        }
    }
    return bad;
}
