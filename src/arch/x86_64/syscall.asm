; SYSCALL entry point.
;
; The CPU leaves us on the user stack with interrupts masked (SFMASK) and rcx = user rip,
; r11 = user rflags. We switch to the thread's kernel stack and build the same register
; frame an interrupt would (struct interrupt_frame), so the dispatcher can read arguments
; and write the result in place, and so that return is a plain iretq. That avoids SYSRET's
; pitfalls (a non-canonical rip faults in ring 0) at the price of a few cycles.

bits 64
extern syscall_dispatch
extern bkl_lock
extern bkl_unlock

CPU_KSTACK_TOP equ 8        ; offsets into struct cpu (percpu.h)
CPU_USER_RSP   equ 16

section .text progbits alloc exec nowrite align=16
global syscall_entry
syscall_entry:
    swapgs
    mov [gs:CPU_USER_RSP], rsp
    mov rsp, [gs:CPU_KSTACK_TOP]

    push qword 0x23             ; ss  (USER_DS)
    push qword [gs:CPU_USER_RSP]
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
    call bkl_lock               ; kernel code runs under the big kernel lock
    mov rdi, rsp
    sti
    call syscall_dispatch
    cli
    call bkl_unlock             ; we always leave through user mode

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
    swapgs
    iretq
