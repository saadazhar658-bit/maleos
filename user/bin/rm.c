#include "ulib.h"

int main(int argc, char **argv)
{
    int status = 0;
    if (argc < 2) {
        dprintf(2, "usage: rm path...\n");
        return 2;
    }
    for (int i = 1; i < argc; i++) {
        long rc = unlink(argv[i]);
        if (rc < 0) {
            dprintf(2, "rm: %s: %s\n", argv[i], strerror((int)-rc));
            status = 1;
        }
    }
    return status;
}
