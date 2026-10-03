; Interrupt entry stubs for all 256 vectors.
;
; Vectors that push an error code: 8, 10-14, 17, 21, 29, 30. For the rest we push
; a dummy 0 so every handler sees the same struct interrupt_frame layout.

bits 64
extern interrupt_dispatch

%macro ISR_NOERR 1
isr_%1:
    push 0
    push %1
    jmp isr_common
%endmacro

%macro ISR_ERR 1
isr_%1:
    push %1
    jmp isr_common
%endmacro

section .text progbits alloc exec nowrite align=16

%assign v 0
%rep 256
    %if v = 8 || (v >= 10 && v <= 14) || v = 17 || v = 21 || v = 29 || v = 30
        ISR_ERR v
    %else
        ISR_NOERR v
    %endif
    %assign v v + 1
%endrep

isr_common:
    push rax
    push rbx
    push rcx
    push rdx
    push rsi
    push rdi
    push rbp
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15

    cld
    mov rdi, rsp                ; struct interrupt_frame *
    call interrupt_dispatch

    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rbp
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rbx
    pop rax
    add rsp, 16                 ; vector + error code
    iretq

section .rodata progbits alloc noexec nowrite align=8
global isr_stub_table
isr_stub_table:
%assign v 0
%rep 256
    dq isr_ %+ v
    %assign v v + 1
%endrep
