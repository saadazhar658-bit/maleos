#include "ulib.h"

/* The heap: grow, use, shrink, grow again (fresh pages must be zero), and hit the limit. */

static int fail(const char *what)
{
    dprintf(2, "t_brk: %s\n", what);
    return 1;
}

int main(void)
{
    long base = sbrk(0);
    if (base <= 0)
        return fail("sbrk(0)");

    uint8_t *p = (uint8_t *)base;
    if (sbrk(10000) != base)
        return fail("grow returns the old break");
    if (sbrk(0) != base + 10000)
        return fail("break moved");
    for (int i = 0; i < 10000; i++) {
        if (p[i] != 0)
            return fail("fresh heap is not zero");
        p[i] = (uint8_t)(i * 7);
    }
    for (int i = 0; i < 10000; i++) {
        if (p[i] != (uint8_t)(i * 7))
            return fail("heap data lost");
    }

    /* Shrink into the middle of a page, then grow back: the freed tail must read as zero. */
    if (sbrk(-9000) != base + 10000)
        return fail("shrink");
    if (sbrk(9000) != base + 1000)
        return fail("regrow");
    for (int i = 1000; i < 10000; i++) {
        if (p[i] != 0)
            return fail("memory came back dirty after shrink+grow");
    }
    for (int i = 0; i < 1000; i++) {
        if (p[i] != (uint8_t)(i * 7))
            return fail("kept part of the heap changed");
    }

    /* The limit is finite and reported cleanly. */
    long grown = 0;
    while (sbrk(65536) >= 0)
        grown += 65536;
    if (grown < 1 << 20)
        return fail("heap limit too small");
    if (sbrk(65536) != -ENOMEM)
        return fail("limit is sticky");
    if (sbrk(-grown) < 0)
        return fail("give everything back");
    return 0;
}
