#include "ulib.h"

/* Overflows a stack buffer on purpose. The stack protector must catch it when the function
 * returns: the program exits with status 134 and never reaches the line after the call. */

static volatile int len = 64;

__attribute__((noinline)) static void victim(const char *src)
{
    char buf[16];
    for (int i = 0; i < len; i++)
        ((volatile char *)buf)[i] = src[i % 8];
    ((volatile char *)buf)[0] = 'x';
}

int main(void)
{
    victim("AAAAAAAA");
    puts("t_smash: the overflow was not detected");
    return 0;
}
