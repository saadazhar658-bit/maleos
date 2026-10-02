; Maleos boot code.
;
; GRUB (Multiboot2) enters at _start in 32-bit protected mode with
;   eax = 0x36D76289, ebx = physical address of the Multiboot2 info structure.
;
; This file:
;   1. validates the CPU (CPUID, long mode, NX)
;   2. builds boot page tables: identity map + higher-half map of the first 1 GiB
;   3. enables PAE, long mode, NX, paging and write-protect (CR0.WP)
;   4. jumps into the higher half and calls kmain(mbi_phys)

MULTIBOOT2_MAGIC  equ 0xE85250D6
MULTIBOOT2_ARCH   equ 0                 ; i386 protected mode
MULTIBOOT2_LOADER equ 0x36D76289

CODE64_SEL        equ 0x08
DATA64_SEL        equ 0x10

section .multiboot_header progbits alloc noexec nowrite align=8
mb2_start:
    dd MULTIBOOT2_MAGIC
    dd MULTIBOOT2_ARCH
    dd mb2_end - mb2_start
    dd 0x100000000 - (MULTIBOOT2_MAGIC + MULTIBOOT2_ARCH + (mb2_end - mb2_start))
    dw 0                                ; end tag
    dw 0
    dd 8
mb2_end:

; ---------------------------------------------------------------------------
; 32-bit entry (runs at its physical address)
; ---------------------------------------------------------------------------
section .boot.text progbits alloc exec nowrite align=16
bits 32
global _start
_start:
    cli
    mov esp, boot_stack_top
    mov esi, ebx                        ; keep MBI pointer (cpuid clobbers ebx)

    cmp eax, MULTIBOOT2_LOADER
    mov al, '1'
    jne boot_error

    ; --- CPUID available? (can we flip EFLAGS.ID) ---
    pushfd
    pop eax
    mov ecx, eax
    xor eax, 1 << 21
    push eax
    popfd
    pushfd
    pop eax
    push ecx
    popfd
    xor eax, ecx
    mov al, '2'
    jz boot_error

    ; --- long mode + NX ---
    mov eax, 0x80000000
    cpuid
    cmp eax, 0x80000001
    mov al, '3'
    jb boot_error
    mov eax, 0x80000001
    cpuid
    test edx, 1 << 29
    mov al, '3'
    jz boot_error
    test edx, 1 << 20
    mov al, '4'
    jz boot_error

    ; --- page tables ---
    ; PML4[0]   -> PDPT_LOW   (identity)
    ; PML4[511] -> PDPT_HIGH  (higher half, 0xFFFFFFFF80000000)
    mov eax, pdpt_low
    or eax, 0x3
    mov [pml4], eax
    mov eax, pdpt_high
    or eax, 0x3
    mov [pml4 + 511 * 8], eax

    ; Both PDPTs point at the same PD that maps physical 0..1 GiB with 2 MiB pages.
    mov eax, pd
    or eax, 0x3
    mov [pdpt_low], eax
    mov [pdpt_high + 510 * 8], eax

    xor ecx, ecx
.fill_pd:
    mov eax, ecx
    shl eax, 21
    or eax, 0x83                        ; present | write | huge
    mov [pd + ecx * 8], eax
    mov dword [pd + ecx * 8 + 4], 0
    inc ecx
    cmp ecx, 512
    jne .fill_pd

    ; --- enable long mode ---
    mov eax, cr4
    or eax, 1 << 5                      ; PAE
    mov cr4, eax

    mov eax, pml4
    mov cr3, eax

    mov ecx, 0xC0000080                 ; EFER
    rdmsr
    or eax, (1 << 8) | (1 << 11)        ; LME | NXE
    wrmsr

    mov eax, cr0
    or eax, (1 << 31) | (1 << 16)       ; PG | WP
    mov cr0, eax

    lgdt [gdt64_ptr]
    jmp CODE64_SEL:long_low

; al = ASCII error code
boot_error:
    mov dword [0xB8000], 0x4F524F45     ; "ER"
    mov dword [0xB8004], 0x4F3A4F52     ; "R:"
    mov ah, 0x4F
    mov [0xB8008], ax
.halt:
    cli
    hlt
    jmp .halt

; ---------------------------------------------------------------------------
; 64-bit, still running at the low (identity-mapped) address
; ---------------------------------------------------------------------------
bits 64
long_low:
    mov ax, DATA64_SEL
    mov ds, ax
    mov es, ax
    mov ss, ax
    xor eax, eax
    mov fs, ax
    mov gs, ax
    mov rax, long_high
    jmp rax

; ---------------------------------------------------------------------------
; Higher half
; ---------------------------------------------------------------------------
section .text progbits alloc exec nowrite align=16
bits 64
extern kmain
global long_high
long_high:
    mov rsp, __stack_top
    xor ebp, ebp
    mov edi, esi                        ; arg0 = MBI physical address
    call kmain
.halt:
    cli
    hlt
    jmp .halt

; ---------------------------------------------------------------------------
; Data
; ---------------------------------------------------------------------------
section .boot.data progbits alloc noexec write align=16
align 16
gdt64:
    dq 0                                ; null
    dq 0x00AF9A000000FFFF               ; 0x08: 64-bit code
    dq 0x00CF92000000FFFF               ; 0x10: data
gdt64_end:
gdt64_ptr:
    dw gdt64_end - gdt64 - 1
    dd gdt64

section .boot.bss nobits alloc noexec write align=4096
align 4096
pml4:       resb 4096
pdpt_low:   resb 4096
pdpt_high:  resb 4096
pd:         resb 4096
boot_stack: resb 4096
boot_stack_top:

; Kernel stack. The lowest page is a guard page: vmm_init() leaves it unmapped,
; so a stack overflow faults instead of silently corrupting memory.
section .bss nobits alloc noexec write align=4096
align 4096
global __stack_guard
global __stack_top
__stack_guard:  resb 4096
__stack_bottom: resb 16384
__stack_top:
