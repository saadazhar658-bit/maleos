#include "ulib.h"

int main(void)
{
    unsigned long ms = uptime_ms();
    printf("up %lu.%03lu seconds\n", ms / 1000, ms % 1000);
    return 0;
}
