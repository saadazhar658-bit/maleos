; SYSCALL entry point.
;
; The CPU leaves us on the user stack with interrupts masked (SFMASK) and rcx = user rip,
; r11 = user rflags. We switch to the thread's kernel stack and build the same register
; frame an interrupt would (struct interrupt_frame), so the dispatcher can read arguments
; and write the result in place, and so that return is a plain iretq. That avoids SYSRET's
; pitfalls (a non-canonical rip faults in ring 0) at the price of a few cycles.

bits 64
extern syscall_dispatch
extern syscall_kstack_top

section .bss align=8
user_rsp_tmp: resq 1

section .text progbits alloc exec nowrite align=16
global syscall_entry
syscall_entry:
    mov [rel user_rsp_tmp], rsp
    mov rsp, [rel syscall_kstack_top]

    push qword 0x23             ; ss  (USER_DS)
    push qword [rel user_rsp_tmp]
    push r11                    ; rflags
    push qword 0x2B             ; cs  (USER_CS)
    push rcx                    ; rip
    push qword 0                ; error code
    push qword 0x80             ; vector
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
    mov rdi, rsp
    sti
    call syscall_dispatch
    cli

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
    add rsp, 16
    iretq
