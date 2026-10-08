#include "drivers/selftest.h"

#include "arch/ioapic.h"
#include "drivers/driver.h"
#include "drivers/kbd.h"
#include "drivers/pci.h"
#include "kernel/acpi.h"
#include "kernel/printk.h"
#include "kernel/sched.h"
#include "kernel/string.h"

static int checks;

#define CHECK(cond, what)                                                                          \
    do {                                                                                           \
        checks++;                                                                                  \
        if (!(cond))                                                                               \
            kpanic("driver selftest failed: %s (%s:%d)", what, __FILE__, __LINE__);                \
    } while (0)

/* Feed a make+break pair for a plain key. */
static void tap(uint8_t make)
{
    kbd_feed_scancode(make);
    kbd_feed_scancode(make | 0x80);
}

static void test_keyboard_decoder(void)
{
    kbd_reset_state();
    CHECK(kbd_trygetc() == -1, "empty after reset");

    tap(0x23); /* h */
    CHECK(kbd_trygetc() == 'h', "plain letter");
    CHECK(kbd_trygetc() == -1, "break code produces nothing");

    kbd_feed_scancode(0x2A); /* left shift down */
    tap(0x1E);
    tap(0x02);
    kbd_feed_scancode(0xAA); /* shift up */
    tap(0x1E);
    CHECK(kbd_trygetc() == 'A', "shift + letter");
    CHECK(kbd_trygetc() == '!', "shift + digit");
    CHECK(kbd_trygetc() == 'a', "shift released");

    tap(0x3A); /* caps lock on */
    tap(0x1E);
    kbd_feed_scancode(0x36); /* right shift */
    tap(0x1E);
    kbd_feed_scancode(0xB6);
    tap(0x02); /* digits are not affected by caps */
    tap(0x3A); /* caps lock off */
    tap(0x1E);
    CHECK(kbd_trygetc() == 'A', "caps lock");
    CHECK(kbd_trygetc() == 'a', "caps + shift cancel out");
    CHECK(kbd_trygetc() == '1', "caps does not touch digits");
    CHECK(kbd_trygetc() == 'a', "caps lock off again");

    kbd_feed_scancode(0x1D); /* ctrl down */
    tap(0x2E);               /* c */
    kbd_feed_scancode(0x9D);
    CHECK(kbd_trygetc() == 3, "ctrl+c is 0x03");

    tap(0x1C);
    tap(0x0E);
    tap(0x0F);
    tap(0x01);
    tap(0x39);
    CHECK(kbd_trygetc() == '\n', "enter");
    CHECK(kbd_trygetc() == '\b', "backspace");
    CHECK(kbd_trygetc() == '\t', "tab");
    CHECK(kbd_trygetc() == 27, "escape");
    CHECK(kbd_trygetc() == ' ', "space");

    kbd_feed_scancode(0xE0);
    kbd_feed_scancode(0x48);
    kbd_feed_scancode(0xE0);
    kbd_feed_scancode(0xC8);
    kbd_feed_scancode(0xE0);
    kbd_feed_scancode(0x53);
    kbd_feed_scancode(0xE0);
    kbd_feed_scancode(0xD3);
    CHECK(kbd_trygetc() == KEY_UP, "extended: up arrow");
    CHECK(kbd_trygetc() == KEY_DELETE, "extended: delete");
    CHECK(kbd_trygetc() == -1, "extended breaks produce nothing");

    kbd_feed_scancode(0xE0); /* an extended prefix must not leak into the next plain key */
    kbd_feed_scancode(0x2A); /* fake shift (sent by some keyboards around arrows) */
    tap(0x1E);
    CHECK(kbd_trygetc() == 'a', "extended prefix state does not leak");

    /* Overflow: the oldest keys are kept, the newest dropped and counted. */
    kbd_reset_state();
    uint64_t dropped0 = kbd_dropped_keys();
    for (int i = 0; i < 200; i++)
        tap(0x1E);
    int got = 0;
    while (kbd_trygetc() >= 0)
        got++;
    CHECK(got == KBD_BUFFER_SIZE - 1, "ring buffer holds size-1 keys");
    CHECK(kbd_dropped_keys() - dropped0 == 200 - (KBD_BUFFER_SIZE - 1), "dropped keys are counted");
    kbd_reset_state();
}

static void key_feeder(void *arg)
{
    (void)arg;
    thread_sleep_ms(30);
    tap(0x2D); /* x */
}

static void test_keyboard_blocking(void)
{
    kbd_reset_state();
    uint64_t t0 = timer_ticks();
    CHECK(kbd_getc_timeout(40) == -1, "timeout with no key");
    CHECK(timer_ticks() - t0 >= 3, "timeout waited");

    struct thread *t = thread_create("key-feeder", key_feeder, NULL, SCHED_PRIO_NORMAL);
    t0 = timer_ticks();
    int k = kbd_getc_timeout(1000);
    CHECK(k == 'x', "a blocked reader is woken by a key");
    CHECK(timer_ticks() - t0 < 30, "wakeup is prompt");
    thread_join(t);
}

static void test_pci(void)
{
    CHECK(pci_device_count() >= 1, "PCI finds devices");
    CHECK(pci_find_class(PCI_CLASS_BRIDGE, 0x00, 0) != NULL, "host bridge present");

    int bad = 0, bars = 0;
    for (int i = 0; i < pci_device_count(); i++) {
        struct pci_dev *d = pci_device(i);
        if (d->vendor == 0xFFFF || d->vendor == 0)
            bad++;
        for (int b = 0; b < 6; b++) {
            if (!d->bar[b].size)
                continue;
            bars++;
            if (d->bar[b].size & (d->bar[b].size - 1)) /* sizes are powers of two */
                bad++;
        }
    }
    CHECK(bad == 0, "device IDs valid, BAR sizes are powers of two");
    CHECK(bars >= 1, "at least one BAR decoded");

    struct pci_dev *h = pci_find_class(PCI_CLASS_BRIDGE, 0x00, 0);
    CHECK(pci_read16(h, 0x00) == h->vendor && pci_read16(h, 0x02) == h->device,
          "16-bit config reads agree with the scan");
}

static void test_ioapic(void)
{
    CHECK(ioapic_count() >= 1, "I/O APIC mapped");
    uint32_t gsi = ioapic_isa_to_gsi(1);
    uint64_t e = ioapic_read_entry(gsi);
    CHECK(e != ~0ULL, "keyboard GSI belongs to an I/O APIC");
    CHECK((e & 0xFF) == 0x31, "keyboard vector programmed");
    CHECK(!(e & (1u << 16)), "keyboard input unmasked");

    uint64_t c = ioapic_read_entry(ioapic_isa_to_gsi(4)); /* COM1: the console routes it */
    CHECK(c != ~0ULL && (c & 0xFF) == 0x32 && !(c & (1u << 16)), "serial input routed");

    uint64_t t = ioapic_read_entry(ioapic_isa_to_gsi(3)); /* COM2: nobody routed it */
    CHECK(t != ~0ULL && (t & (1u << 16)), "unused inputs stay masked");
}

static void test_driver_shim(void)
{
    CHECK(driver_count() >= 1, "drivers registered");
    CHECK(driver_register(NULL) < 0, "NULL driver rejected");
    static const struct driver empty = {.name = "empty"};
    CHECK(driver_register(&empty) < 0, "driver with no entry point rejected");

    int bound = 0;
    for (int i = 0; i < pci_device_count(); i++) {
        struct pci_dev *d = pci_device(i);
        if (d->bound) {
            bound++;
            CHECK(d->owner != NULL, "bound device records its owner");
        }
    }
    (void)bound;
}

int drivers_selftest(void)
{
    checks = 0;
    test_keyboard_decoder();
    test_keyboard_blocking();
    test_pci();
    test_ioapic();
    test_driver_shim();
    return checks;
}
