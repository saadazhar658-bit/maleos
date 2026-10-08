#include "ulib.h"

int main(void)
{
    struct abi_meminfo mi;
    if (meminfo(&mi) < 0)
        return 1;
    unsigned long kb = (unsigned long)(mi.page_size / 1024);
    printf("total %lu KiB, free %lu KiB, used %lu KiB, %u process(es)\n",
           (unsigned long)mi.total_frames * kb, (unsigned long)mi.free_frames * kb,
           (unsigned long)(mi.total_frames - mi.free_frames) * kb, mi.processes);
    return 0;
}
