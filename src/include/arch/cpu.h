#ifndef MALEOS_ARCH_CPU_H
#define MALEOS_ARCH_CPU_H

#include <stdint.h>

static inline void outb(uint16_t port, uint8_t value)
{
    __asm__ volatile("outb %0, %1" : : "a"(value), "Nd"(port));
}

static inline uint8_t inb(uint16_t port)
{
    uint8_t value;
    __asm__ volatile("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

static inline uint64_t read_cr2(void)
{
    uint64_t v;
    __asm__ volatile("mov %%cr2, %0" : "=r"(v));
    return v;
}

static inline uint64_t read_cr3(void)
{
    uint64_t v;
    __asm__ volatile("mov %%cr3, %0" : "=r"(v));
    return v;
}

static inline void write_cr3(uint64_t v)
{
    __asm__ volatile("mov %0, %%cr3" : : "r"(v) : "memory");
}

static inline void invlpg(uint64_t virt)
{
    __asm__ volatile("invlpg (%0)" : : "r"(virt) : "memory");
}

__attribute__((noreturn)) static inline void halt_forever(void)
{
    for (;;)
        __asm__ volatile("cli; hlt");
}

/* QEMU's isa-debug-exit device (no effect on real hardware or plain QEMU runs). */
static inline void qemu_debug_exit(uint8_t code)
{
    outb(0xF4, code);
}

#endif
