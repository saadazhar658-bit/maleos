#include "ulib.h"

int main(int argc, char **argv)
{
    if (argc != 2) {
        dprintf(2, "usage: sleep seconds\n");
        return 2;
    }
    sleep_ms((unsigned long)atol(argv[1]) * 1000);
    return 0;
}
