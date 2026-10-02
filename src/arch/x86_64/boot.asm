; Maleos Phase 0 smoke-test entry point.
;
; This is a deliberately tiny placeholder so the build system, ISO packaging
; and CI boot test can be verified before real kernel work begins (Phase 1).
; It prints a marker to the serial port, then asks QEMU to exit through the
; isa-debug-exit device. Replace it during Phase 1.

MULTIBOOT2_MAGIC  equ 0xE85250D6
MULTIBOOT2_ARCH   equ 0                 ; i386 protected mode
SERIAL_COM1       equ 0x3F8
QEMU_EXIT_PORT    equ 0xF4
QEMU_EXIT_CODE    equ 0x10              ; QEMU exits with (0x10 << 1) | 1 = 33

section .multiboot_header
align 8
mb2_start:
    dd MULTIBOOT2_MAGIC
    dd MULTIBOOT2_ARCH
    dd mb2_end - mb2_start
    dd 0x100000000 - (MULTIBOOT2_MAGIC + MULTIBOOT2_ARCH + (mb2_end - mb2_start))

    ; End tag
    dw 0
    dw 0
    dd 8
mb2_end:

section .text
bits 32
global _start
_start:
    cli
    mov esp, stack_top

    mov esi, boot_msg
.print:
    lodsb
    test al, al
    jz .done
    mov dx, SERIAL_COM1
    out dx, al
    jmp .print

.done:
    ; Signal QEMU to exit (only effective with -device isa-debug-exit).
    mov dx, QEMU_EXIT_PORT
    mov al, QEMU_EXIT_CODE
    out dx, al

.hang:
    cli
    hlt
    jmp .hang

section .rodata
boot_msg: db "MALEOS BOOT OK", 10, 0

section .bss
align 16
stack_bottom:
    resb 16384
stack_top:
