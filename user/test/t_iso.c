#include "ulib.h"

/*
 * Address-space isolation. Many copies run at once, all using the same virtual addresses;
 * each keeps writing its own id there and yielding. Any leak between processes (shared
 * pages, a stale mapping, dirty recycled frames) makes a copy see somebody else's value.
 */

static int global;
static uint8_t table[8192];

int main(int argc, char **argv)
{
    int id = argc > 1 ? (int)atol(argv[1]) : 1;

    if (global != 0)
        return 10;
    for (int i = 0; i < 8192; i++) {
        if (table[i] != 0)
            return 11; /* .bss must start zeroed even though the frames are recycled */
    }
    uint8_t *heap = (uint8_t *)sbrk(8192);
    if ((long)heap < 0)
        return 12;
    for (int i = 0; i < 8192; i++) {
        if (heap[i] != 0)
            return 13;
    }

    for (int i = 0; i < 300; i++) {
        global = id * 100000 + i;
        table[i % 8192] = (uint8_t)id;
        heap[(i * 13) % 8192] = (uint8_t)id;
        yield();
        if (i % 50 == 0)
            sleep_ms(1);
        if (global != id * 100000 + i)
            return 20;
        if (table[i % 8192] != (uint8_t)id)
            return 21;
        if (heap[(i * 13) % 8192] != (uint8_t)id)
            return 22;
    }
    for (int i = 0; i < 8192; i++) {
        if (table[i] != 0 && table[i] != (uint8_t)id)
            return 23;
        if (heap[i] != 0 && heap[i] != (uint8_t)id)
            return 24;
    }
    return 0;
}
