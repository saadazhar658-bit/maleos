/* Link script for Maleos user programs: three page-aligned segments (R+X, R, R+W). */
ENTRY(_start)

PHDRS
{
    text   PT_LOAD FLAGS(5); /* R X */
    rodata PT_LOAD FLAGS(4); /* R   */
    data   PT_LOAD FLAGS(6); /* R W */
}

SECTIONS
{
    . = 0x400000;

    .text : { *(.text .text.*) } :text

    . = ALIGN(4096);
    .rodata : { *(.rodata .rodata.*) } :rodata

    . = ALIGN(4096); /* also when .data is empty: .bss must not share a page with .rodata */
    .data : { *(.data .data.*) } :data
    .bss : { *(.bss .bss.* COMMON) } :data

    /DISCARD/ : { *(.note*) *(.eh_frame*) *(.comment) *(.gnu*) }
}
