#include "ulib.h"

/* Prints a stack address and the initial heap break so the self-test can check that both
 * move from run to run. */

int main(void)
{
    int local = 0;
    printf("%lx %lx\n", (unsigned long)&local, (unsigned long)sbrk(0));
    return 0;
}
