#include "drivers/kbd.h"

#include "arch/cpu.h"
#include "arch/ioapic.h"
#include "arch/irq.h"
#include "drivers/driver.h"
#include "kernel/printk.h"
#include "kernel/sched.h"
#include "kernel/spinlock.h"

/*
 * PS/2 keyboard (Intel 8042). The controller is left in "translation" mode, so whatever
 * the keyboard speaks, we always receive scancode set 1. Decoded keys go into a ring
 * buffer from the interrupt handler; readers sleep on a wait queue.
 */

#define PS2_DATA 0x60
#define PS2_STATUS 0x64
#define PS2_CMD 0x64

#define ST_OUT_FULL (1u << 0)
#define ST_IN_FULL (1u << 1)

#define VEC_KEYBOARD 0x31
#define ISA_IRQ_KBD 1

/* Scancode set 1 -> ASCII, unshifted and shifted, for make codes 0x00-0x39. */
static const char map_plain[0x3A] = {0,   27,  '1',  '2',  '3',  '4', '5', '6',  '7', '8', '9', '0',
                                     '-', '=', '\b', '\t', 'q',  'w', 'e', 'r',  't', 'y', 'u', 'i',
                                     'o', 'p', '[',  ']',  '\n', 0,   'a', 's',  'd', 'f', 'g', 'h',
                                     'j', 'k', 'l',  ';',  '\'', '`', 0,   '\\', 'z', 'x', 'c', 'v',
                                     'b', 'n', 'm',  ',',  '.',  '/', 0,   '*',  0,   ' '};

static const char map_shift[0x3A] = {0,   27,  '!',  '@',  '#',  '$', '%', '^', '&', '*', '(', ')',
                                     '_', '+', '\b', '\t', 'Q',  'W', 'E', 'R', 'T', 'Y', 'U', 'I',
                                     'O', 'P', '{',  '}',  '\n', 0,   'A', 'S', 'D', 'F', 'G', 'H',
                                     'J', 'K', 'L',  ':',  '"',  '~', 0,   '|', 'Z', 'X', 'C', 'V',
                                     'B', 'N', 'M',  '<',  '>',  '?', 0,   '*', 0,   ' '};

static int ring[KBD_BUFFER_SIZE];
static unsigned head, tail; /* tail == head means empty */
static struct waitq readers;
static spinlock_t kbd_lock = SPINLOCK_INIT;

static bool shift, ctrl, alt, caps, extended;
static volatile uint64_t irqs, dropped;

static void push_key(int key)
{
    unsigned next = (head + 1) % KBD_BUFFER_SIZE;
    if (next == tail) {
        dropped++; /* buffer full: drop the newest key rather than overwrite */
        return;
    }
    ring[head] = key;
    head = next;
    waitq_wake_one(&readers);
}

static int decode_extended(uint8_t make)
{
    switch (make) {
    case 0x48:
        return KEY_UP;
    case 0x50:
        return KEY_DOWN;
    case 0x4B:
        return KEY_LEFT;
    case 0x4D:
        return KEY_RIGHT;
    case 0x47:
        return KEY_HOME;
    case 0x4F:
        return KEY_END;
    case 0x49:
        return KEY_PGUP;
    case 0x51:
        return KEY_PGDN;
    case 0x52:
        return KEY_INSERT;
    case 0x53:
        return KEY_DELETE;
    case 0x1C:
        return '\n'; /* keypad enter */
    case 0x35:
        return '/'; /* keypad slash */
    default:
        return 0;
    }
}

/* Caller holds kbd_lock with interrupts off. */
static void feed_locked(uint8_t code)
{
    if (code == 0xE0) {
        extended = true;
        return;
    }
    /* 0xAA (self-test pass) is deliberately NOT filtered: in set 1 it is also the release
     * of the left shift key, and we never send the reset command that would produce it. */
    if (code == 0xE1 || code == 0xFA || code == 0xFE)
        return; /* pause prefix, ACK, resend: not keys */

    bool release = code & 0x80;
    uint8_t make = code & 0x7F;
    bool was_ext = extended;
    extended = false;

    if (!was_ext) {
        switch (make) {
        case 0x2A:
        case 0x36:
            shift = !release;
            return;
        case 0x1D:
            ctrl = !release;
            return;
        case 0x38:
            alt = !release;
            return;
        case 0x3A:
            if (!release)
                caps = !caps;
            return;
        }
    } else if (make == 0x1D || make == 0x38) { /* right ctrl / alt-gr */
        if (make == 0x1D)
            ctrl = !release;
        else
            alt = !release;
        return;
    }

    if (release)
        return;

    int key = 0;
    if (was_ext) {
        key = decode_extended(make);
    } else if (make < sizeof(map_plain)) {
        char c = map_plain[make];
        bool letter = c >= 'a' && c <= 'z';
        bool upper = letter ? (shift != caps) : shift;
        key = upper ? map_shift[make] : c;
        if (ctrl && letter)
            key = c - 'a' + 1; /* Ctrl+A = 1 ... Ctrl+Z = 26 */
    }
    (void)alt;
    if (key)
        push_key(key);
}

void kbd_feed_scancode(uint8_t code)
{
    uint64_t f = spin_lock_irqsave(&kbd_lock);
    feed_locked(code);
    spin_unlock_irqrestore(&kbd_lock, f);
}

void kbd_reset_state(void)
{
    uint64_t f = spin_lock_irqsave(&kbd_lock);
    head = tail = 0;
    shift = ctrl = alt = caps = extended = false;
    spin_unlock_irqrestore(&kbd_lock, f);
}

static void kbd_irq(struct interrupt_frame *frame)
{
    (void)frame;
    irqs++;
    uint64_t f = spin_lock_irqsave(&kbd_lock);
    for (int i = 0; i < 16 && (inb(PS2_STATUS) & ST_OUT_FULL); i++)
        feed_locked(inb(PS2_DATA));
    spin_unlock_irqrestore(&kbd_lock, f);
}

static int pop_locked(void)
{
    if (head == tail)
        return -1;
    int k = ring[tail];
    tail = (tail + 1) % KBD_BUFFER_SIZE;
    return k;
}

int kbd_trygetc(void)
{
    uint64_t f = spin_lock_irqsave(&kbd_lock);
    int k = pop_locked();
    spin_unlock_irqrestore(&kbd_lock, f);
    return k;
}

int kbd_getc_timeout(uint64_t timeout_ms)
{
    uint64_t deadline = timer_ticks() + ms_to_ticks(timeout_ms) + 1;
    uint64_t f = spin_lock_irqsave(&kbd_lock);
    int k;
    while ((k = pop_locked()) < 0) {
        uint64_t wait = 0;
        if (timeout_ms != UINT64_MAX) {
            uint64_t now = timer_ticks();
            if (now >= deadline)
                break;
            wait = deadline - now;
        }
        waitq_wait(&readers, &kbd_lock, wait);
    }
    spin_unlock_irqrestore(&kbd_lock, f);
    return k;
}

int kbd_getc(void)
{
    return kbd_getc_timeout(UINT64_MAX);
}

uint64_t kbd_irq_count(void)
{
    return irqs;
}

uint64_t kbd_dropped_keys(void)
{
    return dropped;
}

/* ---- controller setup ---- */

static bool wait_input_clear(void)
{
    for (int i = 0; i < 100000; i++) {
        if (!(inb(PS2_STATUS) & ST_IN_FULL))
            return true;
    }
    return false;
}

static bool wait_output_full(void)
{
    for (int i = 0; i < 100000; i++) {
        if (inb(PS2_STATUS) & ST_OUT_FULL)
            return true;
    }
    return false;
}

static void flush_output(void)
{
    for (int i = 0; i < 64 && (inb(PS2_STATUS) & ST_OUT_FULL); i++)
        (void)inb(PS2_DATA);
}

static bool ctl_cmd(uint8_t cmd)
{
    if (!wait_input_clear())
        return false;
    outb(PS2_CMD, cmd);
    return true;
}

static int kbd_init(void)
{
    waitq_init(&readers);
    kbd_reset_state();

    if (!ctl_cmd(0xAD) || !ctl_cmd(0xA7)) /* disable both ports while we configure */
        return -1;
    flush_output();

    if (!ctl_cmd(0x20) || !wait_output_full()) /* read configuration byte */
        return -2;
    uint8_t cfg = inb(PS2_DATA);
    cfg |= 1u << 0;    /* enable IRQ1 */
    cfg &= ~(1u << 1); /* no mouse IRQ */
    cfg |= 1u << 6;    /* translation: always deliver scancode set 1 */
    cfg &= ~(1u << 4); /* make sure the keyboard clock is enabled */
    if (!ctl_cmd(0x60) || !wait_input_clear())
        return -3;
    outb(PS2_DATA, cfg);

    if (!ctl_cmd(0xAE)) /* enable the keyboard port */
        return -4;
    flush_output();

    if (wait_input_clear()) {
        outb(PS2_DATA, 0xF4); /* keyboard: enable scanning */
        if (wait_output_full())
            (void)inb(PS2_DATA); /* consume the ACK */
    }

    /*
     * The 8042 raises no new interrupt while its output buffer is full, so anything left
     * unread here would silence the keyboard forever. Drain it before and after routing.
     */
    flush_output();
    irq_register(VEC_KEYBOARD, kbd_irq);
    if (ioapic_route_isa(ISA_IRQ_KBD, VEC_KEYBOARD) != 0)
        return -5;
    flush_output();
    return 0;
}

const struct driver kbd_driver = {
    .name = "ps2-keyboard",
    .init = kbd_init,
};
