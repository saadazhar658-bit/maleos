#include "ulib.h"

int main(int argc, char **argv);

/* Entry point: the kernel leaves argc, argv[] and a NULL on the stack (see process.c). */
__asm__(".section .text\n"
        ".global _start\n"
        "_start:\n"
        "    xor %ebp, %ebp\n"
        "    pop %rdi\n"
        "    mov %rsp, %rsi\n"
        "    and $-16, %rsp\n"
        "    call __ustart\n"
        "    ud2\n");

void __ustart(int argc, char **argv);
void __ustart(int argc, char **argv)
{
    exit(main(argc, argv));
}
