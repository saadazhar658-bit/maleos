#ifndef MALEOS_MM_SELFTEST_H
#define MALEOS_MM_SELFTEST_H

/* Runs PMM/VMM/heap checks. Panics on the first failure. Returns the number of checks run. */
int mm_selftest(void);

#endif
