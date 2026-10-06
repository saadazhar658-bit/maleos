#ifndef MALEOS_DRIVERS_KBD_H
#define MALEOS_DRIVERS_KBD_H

#include <stdint.h>

/* Values returned by kbd_getc(): ASCII for printable keys and controls, or one of these. */
#define KEY_UP 0x101
#define KEY_DOWN 0x102
#define KEY_LEFT 0x103
#define KEY_RIGHT 0x104
#define KEY_HOME 0x105
#define KEY_END 0x106
#define KEY_PGUP 0x107
#define KEY_PGDN 0x108
#define KEY_INSERT 0x109
#define KEY_DELETE 0x10A

#define KBD_BUFFER_SIZE 128

/* Blocking read of the next key press. */
int kbd_getc(void);
/* Wait up to timeout_ms; returns -1 on timeout. */
int kbd_getc_timeout(uint64_t timeout_ms);
/* Non-blocking: returns -1 if no key is waiting. */
int kbd_trygetc(void);

/*
 * Feed one raw scancode (set 1) to the decoder, exactly as the interrupt handler does.
 * Exposed so the decoder can be tested without hardware.
 */
void kbd_feed_scancode(uint8_t code);
/* Drop buffered keys and reset modifier state. */
void kbd_reset_state(void);

/* Counters for the self-tests. */
uint64_t kbd_irq_count(void);
uint64_t kbd_dropped_keys(void);

extern const struct driver kbd_driver;

#endif
