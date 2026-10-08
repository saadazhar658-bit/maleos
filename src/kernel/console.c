#include "kernel/console.h"

#include "arch/cpu.h"
#include "arch/ioapic.h"
#include "arch/irq.h"
#include "drivers/kbd.h"
#include "kernel/printk.h"
#include "kernel/sched.h"
#include "kernel/spinlock.h"
#include "kernel/sync.h"

#define COM1 0x3F8
#define VEC_SERIAL 0x32
#define ISA_IRQ_SERIAL 4
#define RING_SIZE 256
#define LINE_MAX 256

static char ring[RING_SIZE];
static size_t ring_head, ring_tail, ring_count;
static spinlock_t ring_lock = SPINLOCK_INIT;
static struct waitq ring_wq;

static struct mutex reader_lock;
static char line[LINE_MAX];
static size_t line_len, line_pos;
static bool last_was_cr;

static void ring_put(char c)
{
    uint64_t f = spin_lock_irqsave(&ring_lock);
    if (ring_count < RING_SIZE) {
        ring[ring_head] = c;
        ring_head = (ring_head + 1) % RING_SIZE;
        ring_count++;
        waitq_wake_one(&ring_wq);
    }
    spin_unlock_irqrestore(&ring_lock, f);
}

static char ring_get(void)
{
    uint64_t f = spin_lock_irqsave(&ring_lock);
    while (ring_count == 0)
        waitq_wait(&ring_wq, &ring_lock, 0);
    char c = ring[ring_tail];
    ring_tail = (ring_tail + 1) % RING_SIZE;
    ring_count--;
    spin_unlock_irqrestore(&ring_lock, f);
    return c;
}

void console_feed(const char *s, size_t n)
{
    for (size_t i = 0; i < n; i++)
        ring_put(s[i]);
}

void console_flush_input(void)
{
    uint64_t f = spin_lock_irqsave(&ring_lock);
    ring_head = ring_tail = ring_count = 0;
    spin_unlock_irqrestore(&ring_lock, f);
}

static void serial_irq(struct interrupt_frame *frame)
{
    (void)frame;
    while (inb(COM1 + 5) & 1) {
        char c = (char)inb(COM1);
        if (c == '\r') {
            last_was_cr = true;
            c = '\n';
        } else if (c == '\n' && last_was_cr) {
            last_was_cr = false; /* the LF of a CR LF pair */
            continue;
        } else {
            last_was_cr = false;
        }
        ring_put(c);
    }
}

void console_init(void)
{
    waitq_init(&ring_wq);
    mutex_init(&reader_lock);
    irq_register(VEC_SERIAL, serial_irq);
    if (ioapic_route_isa(ISA_IRQ_SERIAL, VEC_SERIAL) == 0) {
        while (inb(COM1 + 5) & 1)
            (void)inb(COM1);  /* discard anything received before we were listening */
        outb(COM1 + 1, 0x01); /* interrupt when a byte arrives */
    }
}

static void kbd_pump(void *arg)
{
    (void)arg;
    for (;;) {
        int k = kbd_getc();
        if (k > 0 && k < 0x100)
            ring_put((char)k);
    }
}

void console_start(void)
{
    thread_detach(thread_create("console-in", kbd_pump, NULL, SCHED_PRIO_NORMAL));
}

void console_write(const char *buf, size_t n)
{
    printk_write(buf, n);
}

int console_read(char *buf, size_t n)
{
    if (n == 0)
        return 0;
    mutex_lock(&reader_lock);

    if (line_pos >= line_len) { /* nothing left over: read a new line */
        line_len = line_pos = 0;
        for (;;) {
            char c = ring_get();
            if (c == '\n') {
                console_write("\n", 1);
                line[line_len++] = '\n';
                break;
            }
            if (c == 4) { /* Ctrl-D */
                if (line_len == 0) {
                    mutex_unlock(&reader_lock);
                    return 0;
                }
                break;
            }
            if (c == 3) { /* Ctrl-C: abandon the line */
                console_write("^C\n", 3);
                line_len = 0;
                line[line_len++] = '\n';
                break;
            }
            if (c == 8 || c == 0x7F) {
                if (line_len > 0) {
                    line_len--;
                    console_write("\b \b", 3);
                }
                continue;
            }
            if (c >= 0x20 && c < 0x7F && line_len < LINE_MAX - 1) {
                line[line_len++] = c;
                console_write(&c, 1);
            }
        }
    }

    size_t chunk = line_len - line_pos;
    if (chunk > n)
        chunk = n;
    for (size_t i = 0; i < chunk; i++)
        buf[i] = line[line_pos + i];
    line_pos += chunk;
    mutex_unlock(&reader_lock);
    return (int)chunk;
}
