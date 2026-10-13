; SMP support code: the application-processor start-up trampoline and the idle wait.
;
; A started AP begins in 16-bit real mode at the physical page the SIPI names. The blob
; between ap_tramp_start and ap_tramp_end is copied to TRAMP_PHYS, then the BSP fills in the
; data block (ap_data_*) and sends INIT/SIPI. The AP climbs real -> protected -> long mode
; on the boot page tables (identity map of the first GiB plus the higher half), loads its
; stack, per-CPU pointer and the real kernel CR3 from the data block, and calls ap_main().

TRAMP_PHYS equ 0x8000
%define T(x) (TRAMP_PHYS + (x) - ap_tramp_start)

bits 64
extern ap_main
extern bkl_lock
extern bkl_unlock

global ap_tramp_start
global ap_tramp_end
global ap_data_pml4
global ap_data_stack
global ap_data_cpu
global ap_data_entry
global ap_data_cr3
global ap_high
global cpu_idle_wait
global idle_window_start
global idle_window_end

section .rodata.ap_tramp progbits alloc noexec nowrite align=16
ap_tramp_start:
bits 16
    cli
    cld
    xor ax, ax
    mov ds, ax
    lgdt [T(ap_gdt_ptr)]
    mov eax, cr0
    or eax, 1                           ; PE
    mov cr0, eax
    jmp dword 0x08:T(ap_pm32)

bits 32
ap_pm32:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov eax, cr4
    or eax, 1 << 5                      ; PAE
    mov cr4, eax
    mov eax, [T(ap_data_pml4)]
    mov cr3, eax
    mov ecx, 0xC0000080                 ; EFER
    rdmsr
    or eax, (1 << 8) | (1 << 11)        ; LME | NXE
    wrmsr
    mov eax, cr0
    or eax, (1 << 31) | (1 << 16)       ; PG | WP
    mov cr0, eax
    jmp 0x18:T(ap_lm64)

bits 64
ap_lm64:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov eax, T(ap_data_pml4)
    mov rdi, [rax + (ap_data_cpu - ap_data_pml4)]
    mov rsi, [rax + (ap_data_cr3 - ap_data_pml4)]
    mov rsp, [rax + (ap_data_stack - ap_data_pml4)]
    mov rax, [rax + (ap_data_entry - ap_data_pml4)]
    jmp rax

align 16
ap_gdt:
    dq 0
    dq 0x00CF9A000000FFFF               ; 0x08: 32-bit code
    dq 0x00CF92000000FFFF               ; 0x10: data
    dq 0x00AF9A000000FFFF               ; 0x18: 64-bit code
ap_gdt_end:
ap_gdt_ptr:
    dw ap_gdt_end - ap_gdt - 1
    dd T(ap_gdt)

align 8
ap_data_pml4:  dq 0                     ; physical address of the boot PML4 (below 4 GiB)
ap_data_stack: dq 0
ap_data_cpu:   dq 0
ap_data_entry: dq 0
ap_data_cr3:   dq 0
ap_tramp_end:

section .text progbits alloc exec nowrite align=16
bits 64
; Entered (higher half, boot page tables) with rdi = struct cpu *, rsi = kernel CR3.
ap_high:
    mov cr3, rsi
    xor ebp, ebp
    call ap_main
.halt:
    cli
    hlt
    jmp .halt

; void cpu_idle_wait(void): called with the big kernel lock held. Drops it, waits for an
; interrupt, takes it again. Interrupts that land exactly on the `hlt` or the instruction
; after it are recognised by smp_frame_unlocked() as "running without the lock".
cpu_idle_wait:
    cli                                 ; no interrupt may see "lock dropped, rip outside window"
    sub rsp, 8
    call bkl_unlock
    sti
idle_window_start:
    hlt
idle_window_end:
    cli
    call bkl_lock
    add rsp, 8
    ret
