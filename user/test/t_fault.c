#include "ulib.h"

/*
 * Misbehaves on purpose. The kernel must kill this process (not itself) with the exit status
 * matching the CPU exception: 139 for memory/protection faults, 132 for an invalid
 * instruction, 136 for a division by zero.
 */

static volatile int never = 0;    /* keeps the compiler from proving the recursion infinite */
static volatile uintptr_t target; /* ... and from folding constant addresses */

static int recurse(int depth)
{
    volatile char pad[4096];
    pad[0] = (char)depth;
    if (never)
        return 0;
    return recurse(depth + 1) + pad[0];
}

int main(int argc, char **argv)
{
    const char *what = argc > 1 ? argv[1] : "";
    if (!strcmp(what, "null")) {
        target = 0;
        *(volatile int *)target = 1;
    } else if (!strcmp(what, "nullread")) {
        target = 8;
        return *(volatile int *)target;
    } else if (!strcmp(what, "kread")) { /* the kernel's direct map */
        target = 0xFFFF800000000000ULL;
        return *(volatile int *)target;
    } else if (!strcmp(what, "kwrite")) { /* kernel text */
        target = 0xFFFFFFFF80100000ULL;
        *(volatile int *)target = 1;
    } else if (!strcmp(what, "kjump")) {
        target = 0xFFFFFFFF80100000ULL;
        ((void (*)(void))target)();
    } else if (!strcmp(what, "text")) { /* own code is read-only */
        *(volatile uint8_t *)main = 0x90;
    } else if (!strcmp(what, "stackexec")) { /* no code on the stack (NX) */
        uint8_t code[1] = {0xC3};
        ((void (*)(void))code)();
    } else if (!strcmp(what, "stack")) { /* runs into the guard page */
        return recurse(0);
    } else if (!strcmp(what, "cli")) {
        __asm__ volatile("cli");
    } else if (!strcmp(what, "hlt")) {
        __asm__ volatile("hlt");
    } else if (!strcmp(what, "io")) {
        __asm__ volatile("inb $0x60, %%al" : : : "al");
    } else if (!strcmp(what, "cr3")) {
        uint64_t v;
        __asm__ volatile("mov %%cr3, %0" : "=r"(v));
    } else if (!strcmp(what, "msr")) {
        __asm__ volatile("wrmsr" : : "c"(0xC0000082u), "a"(0), "d"(0));
    } else if (!strcmp(what, "ud")) {
        __asm__ volatile("ud2");
    } else if (!strcmp(what, "div")) {
        /* inline asm: the compiler treats a C division by zero as undefined and may drop it */
        __asm__ volatile("xor %%edx, %%edx; mov $1, %%eax; xor %%ecx, %%ecx; div %%ecx"
                         :
                         :
                         : "eax", "ecx", "edx");
    } else if (!strcmp(what, "int80")) { /* interrupt gates are not reachable from ring 3 */
        __asm__ volatile("int $0x80");
    } else if (!strcmp(what, "int30")) { /* ... including the timer's */
        __asm__ volatile("int $0x30");
    } else if (!strcmp(what, "segment")) { /* loading a kernel selector */
        __asm__ volatile("mov $0x10, %%ax; mov %%ax, %%ds" : : : "ax");
    }
    return 0; /* survived: the test fails */
}
