/* Maleos kernel linker script (Phase 0 smoke-test layout). */
ENTRY(_start)

PHDRS
{
    text   PT_LOAD FLAGS(5);   /* R-X */
    rodata PT_LOAD FLAGS(4);   /* R-- */
    data   PT_LOAD FLAGS(6);   /* RW- */
}

SECTIONS
{
    . = 1M;

    .boot : ALIGN(8)
    {
        KEEP(*(.multiboot_header))
    } :text

    .text : ALIGN(4K)
    {
        *(.text .text.*)
    } :text

    .rodata : ALIGN(4K)
    {
        *(.rodata .rodata.*)
    } :rodata

    .data : ALIGN(4K)
    {
        *(.data .data.*)
    } :data

    .bss : ALIGN(4K)
    {
        *(COMMON)
        *(.bss .bss.*)
    } :data

    /DISCARD/ : { *(.comment) *(.eh_frame) *(.note*) }
}
