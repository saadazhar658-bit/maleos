#include "ulib.h"

int main(int argc, char **argv)
{
    printf("Hello from user space! I am pid %ld", getpid());
    for (int i = 1; i < argc; i++)
        printf(" %s", argv[i]);
    printf("\n");
    return 0;
}
