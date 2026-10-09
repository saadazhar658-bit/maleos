#include "ulib.h"

int main(int argc, char **argv);

/* Stack-protector canary (-mstack-protector-guard=global); replaced with random bytes before
 * main() runs. */
unsigned long __stack_chk_guard = 0x595e9fbd94fda766UL;

__attribute__((noreturn)) void __stack_chk_fail(void)
{
    static const char msg[] = "*** stack smashing detected ***\n";
    write(2, msg, sizeof(msg) - 1);
    exit(134);
}

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
__attribute__((no_stack_protector)) void __ustart(int argc, char **argv)
{
    unsigned long g;
    if (getrandom(&g, sizeof(g)) == (long)sizeof(g))
        __stack_chk_guard = g & ~0xffUL;
    exit(main(argc, argv));
}
