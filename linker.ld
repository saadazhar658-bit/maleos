/*
 * Maleos kernel linker script.
 *
 * The kernel is loaded by GRUB at 1 MiB (physical) in 32-bit protected mode.
 * The .boot.* sections run at their physical addresses (VMA == LMA) and set up
 * long mode. Everything else is linked in the higher half:
 *
 *     virtual = KERNEL_VMA + physical
 */
KERNEL_VMA = 0xFFFFFFFF80000000;

ENTRY(_start)

PHDRS
{
    boot_text PT_LOAD FLAGS(5);   /* R-X  low  */
    boot_data PT_LOAD FLAGS(6);   /* RW-  low  */
    text      PT_LOAD FLAGS(5);   /* R-X  high */
    rodata    PT_LOAD FLAGS(4);   /* R--  high */
    data      PT_LOAD FLAGS(6);   /* RW-  high */
}

SECTIONS
{
    . = 1M;
    __kernel_phys_start = .;

    .boot.text : ALIGN(8)
    {
        KEEP(*(.multiboot_header))
        *(.boot.text)
    } :boot_text

    . = ALIGN(4K);
    .boot.data : { *(.boot.data) } :boot_data

    . = ALIGN(4K);
    .boot.bss : { *(.boot.bss) } :boot_data

    /* Switch to the higher half. LMA keeps following the physical layout. */
    . = ALIGN(4K);
    . += KERNEL_VMA;

    .text : AT(ADDR(.text) - KERNEL_VMA)
    {
        *(.text .text.*)
    } :text
    __text_start = ADDR(.text);
    __text_end   = ADDR(.text) + SIZEOF(.text);

    . = ALIGN(4K);
    .rodata : AT(ADDR(.rodata) - KERNEL_VMA)
    {
        *(.rodata .rodata.*)
    } :rodata
    __rodata_start = ADDR(.rodata);
    __rodata_end   = ADDR(.rodata) + SIZEOF(.rodata);

    . = ALIGN(4K);
    .data : AT(ADDR(.data) - KERNEL_VMA)
    {
        *(.data .data.*)
    } :data
    __data_start = ADDR(.data);

    .bss : AT(ADDR(.bss) - KERNEL_VMA)
    {
        *(COMMON)
        *(.bss .bss.*)
    } :data

    . = ALIGN(4K);
    __data_end = .;                          /* end of data + bss (page aligned) */
    __kernel_phys_end = . - KERNEL_VMA;

    /DISCARD/ : { *(.comment) *(.eh_frame) *(.note*) }
}
