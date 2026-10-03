; void context_switch(struct context **old, struct context *new)
;
; Saves the callee-saved registers on the current stack, stores the stack pointer
; in *old, switches to the stack in `new` and restores its registers. Everything
; else (caller-saved registers, flags) is handled by the C calling convention or
; by the interrupt frame the thread was preempted in.

bits 64
section .text progbits alloc exec nowrite align=16

global context_switch
context_switch:
    push rbp
    push rbx
    push r12
    push r13
    push r14
    push r15
    mov [rdi], rsp
    mov rsp, rsi
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbx
    pop rbp
    ret
