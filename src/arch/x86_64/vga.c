#include "arch/vga.h"

#include "kernel/string.h"

#define VGA_COLS 80
#define VGA_ROWS 25
#define VGA_COLOR 0x0F00 /* white on black */

/* Identity-mapped until vmm_init(); the kernel then switches to the HHDM address. */
static uint16_t *vga_base = (uint16_t *)0xB8000;
static int cur_row, cur_col;

void vga_set_base(uint16_t *base)
{
    vga_base = base;
}

void vga_init(void)
{
    for (int i = 0; i < VGA_COLS * VGA_ROWS; i++)
        vga_base[i] = VGA_COLOR | ' ';
    cur_row = cur_col = 0;
}

static void scroll(void)
{
    memmove(vga_base, vga_base + VGA_COLS, (VGA_ROWS - 1) * VGA_COLS * sizeof(uint16_t));
    for (int i = 0; i < VGA_COLS; i++)
        vga_base[(VGA_ROWS - 1) * VGA_COLS + i] = VGA_COLOR | ' ';
    cur_row = VGA_ROWS - 1;
}

void vga_putc(char c)
{
    if (c == '\n') {
        cur_col = 0;
        cur_row++;
    } else if (c == '\r') {
        cur_col = 0;
    } else {
        vga_base[cur_row * VGA_COLS + cur_col] = VGA_COLOR | (uint8_t)c;
        if (++cur_col == VGA_COLS) {
            cur_col = 0;
            cur_row++;
        }
    }
    if (cur_row >= VGA_ROWS)
        scroll();
}
