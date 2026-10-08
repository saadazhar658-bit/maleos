#include "ulib.h"

int main(int argc, char **argv)
{
    if (argc != 4 || strcmp(argv[1], "-s") != 0) {
        dprintf(2, "usage: ln -s target linkname\n");
        return 2;
    }
    long rc = symlink(argv[2], argv[3]);
    if (rc < 0) {
        dprintf(2, "ln: %s: %s\n", argv[3], strerror((int)-rc));
        return 1;
    }
    return 0;
}
