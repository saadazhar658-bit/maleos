#include "kernel/random.h"

#include "arch/cpu.h"
#include "kernel/printk.h"
#include "kernel/spinlock.h"

/* The compiler reads this on every protected function entry and exit
 * (-mstack-protector-guard=global). The initial value only covers the first few boot calls. */
uintptr_t __stack_chk_guard = 0x595e9fbd94fda766UL;

static uint64_t state[4] = {0x9e3779b97f4a7c15UL, 0xbf58476d1ce4e5b9UL, 0x94d049bb133111ebUL,
                            0x2545f4914f6cdd1dUL};
static bool hw_seeded;
static spinlock_t lock = SPINLOCK_INIT;

static inline uint64_t rotl(uint64_t x, int k)
{
    return (x << k) | (x >> (64 - k));
}

static inline uint64_t rdtsc(void)
{
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

static bool rdrand64(uint64_t *out)
{
    for (int i = 0; i < 10; i++) {
        uint64_t v;
        unsigned char ok;
        __asm__ volatile("rdrand %0; setc %1" : "=r"(v), "=qm"(ok)::"cc");
        if (ok) {
            *out = v;
            return true;
        }
    }
    return false;
}

static uint64_t splitmix(uint64_t *x)
{
    uint64_t z = (*x += 0x9e3779b97f4a7c15UL);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9UL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebUL;
    return z ^ (z >> 31);
}

static uint64_t next_locked(void)
{
    uint64_t result = rotl(state[1] * 5, 7) * 9;
    uint64_t t = state[1] << 17;
    state[2] ^= state[0];
    state[3] ^= state[1];
    state[1] ^= state[2];
    state[0] ^= state[3];
    state[2] ^= t;
    state[3] = rotl(state[3], 45);
    return result;
}

uint64_t random_u64(void)
{
    uint64_t flags = spin_lock_irqsave(&lock);
    uint64_t v = next_locked();
    /* Stir in the cycle counter so repeated boots of a deterministic VM still diverge. */
    state[0] ^= rdtsc();
    spin_unlock_irqrestore(&lock, flags);
    return v;
}

void random_bytes(void *buf, size_t len)
{
    uint8_t *p = buf;
    while (len) {
        uint64_t v = random_u64();
        size_t n = len < 8 ? len : 8;
        for (size_t i = 0; i < n; i++)
            p[i] = (uint8_t)(v >> (8 * i));
        p += n;
        len -= n;
    }
}

bool random_hw_seeded(void)
{
    return hw_seeded;
}

__attribute__((no_stack_protector)) void random_init(void)
{
    uint32_t a, b, c, d;
    cpuid(1, &a, &b, &c, &d);
    bool have_rdrand = c & (1u << 30);

    uint64_t seed = rdtsc();
    int got = 0;
    for (int i = 0; i < 4; i++) {
        uint64_t v;
        if (have_rdrand && rdrand64(&v)) {
            got++;
            seed ^= v;
            state[i] ^= v;
        } else {
            state[i] ^= splitmix(&seed);
        }
    }
    hw_seeded = got == 4;
    for (int i = 0; i < 16; i++) /* discard the weak first outputs */
        next_locked();

    /* One byte of the canary is zero so string overflows cannot write it. */
    __stack_chk_guard = (uintptr_t)(next_locked() & ~0xffUL);
    printk("Random: seeded from %s, stack canary armed\n",
           hw_seeded ? "RDRAND" : "the cycle counter");
}

__attribute__((noreturn)) void __stack_chk_fail(void)
{
    kpanic("stack smashing detected");
}
