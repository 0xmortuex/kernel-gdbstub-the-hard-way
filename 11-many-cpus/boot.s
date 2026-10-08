# boot.s - multiboot entry for the 64-bit course kernel (Modules 10-11).
#
# QEMU's -kernel loader (and GRUB) only start multiboot kernels in 32-bit
# protected mode, and QEMU refuses to load an ELF64 image at all. So:
#   - the header sets the "a.out kludge" flag (bit 16): the loader takes the
#     load addresses from the header instead of parsing the file, and we
#     boot a flat binary made with objcopy -O binary (build.py does that);
#   - _start runs as 32-bit code, builds page tables, turns on long mode and
#     far-jumps into 64-bit code.

.set MAGIC, 0x1BADB002
.set FLAGS, 0x00010000                  # bit 16: use the address fields below
.section .multiboot, "a"
.align 4
mb_header:
.long MAGIC
.long FLAGS
.long -(MAGIC + FLAGS)
.long mb_header                         # header_addr
.long __load_start                      # load_addr: first byte of the image
.long __load_end                        # load_end_addr: end of .data
.long __bss_end                         # bss_end_addr: loader zeroes up to here
.long _start                            # entry_addr

.section .bss
.align 4096
.global pml4
pml4:     .skip 4096
pdpt:     .skip 4096
pd_low:   .skip 4096                    # 0 - 1 GiB, 2 MiB pages
pd_high:  .skip 4096                    # 3 - 4 GiB: only the local APIC page
.align 16
stack_bottom:
.skip 16384
stack_top:

.section .text
.code32
.global _start
.type _start, @function
_start:
    cli
    mov $stack_top, %esp

    # Identity-map the first GiB with 512 2 MiB pages. Everything above is
    # unmapped, so the stub's fault-safe reads (Module 05) still have
    # something to fault on: 0x40000000 is the first unmapped byte.
    mov $pdpt, %eax
    or $0x3, %eax                       # present | writable
    mov %eax, pml4
    mov $pd_low, %eax
    or $0x3, %eax
    mov %eax, pdpt
    mov $pd_low, %edi
    mov $0x83, %eax                     # present | writable | 2 MiB page
    mov $512, %ecx
1:  mov %eax, (%edi)
    movl $0, 4(%edi)
    add $0x200000, %eax
    add $8, %edi
    loop 1b
    # The local APIC's registers live at 0xFEE00000, in the fourth GiB.
    # Map just that 2 MiB page, uncached (PCD | PWT).
    mov $pd_high, %eax
    or $0x3, %eax
    mov %eax, pdpt + 3 * 8
    movl $0xFEE0009B, pd_high + 503 * 8 # (0xFEE00000-0xC0000000) >> 21 = 503

.global enter_long_mode                 # also the tail of the AP trampoline
enter_long_mode:
    mov %cr4, %eax
    or $0x20, %eax                      # CR4.PAE
    mov %eax, %cr4
    mov $pml4, %eax
    mov %eax, %cr3
    mov $0xC0000080, %ecx               # EFER
    rdmsr
    or $0x100, %eax                     # EFER.LME
    wrmsr
    mov %cr0, %eax
    or $0x80000001, %eax                # CR0.PG | CR0.PE: long mode is now active
    mov %eax, %cr0
    lgdt gdt64_ptr
    ljmp $0x08, $long_start             # a 64-bit code segment: we're in long mode

.code64
.global long_start                      # global, so backtraces can name it
.type long_start, @function
long_start:
    mov $0x10, %ax
    mov %ax, %ds
    mov %ax, %es
    mov %ax, %ss
    mov %ax, %fs
    mov %ax, %gs
    cmpl $0, ap_booting                 # an AP coming through the trampoline?
    jne ap_entry
    mov $stack_top, %rsp                # upper half of RSP is undefined after the switch
    xor %ebp, %ebp                      # end of the frame-pointer chain
    call kmain
2:  hlt
    jmp 2b

.section .rodata
.align 8
gdt64:
    .quad 0                             # null
    .quad 0x00AF9A000000FFFF            # 0x08: 64-bit code (L=1), DPL0
    .quad 0x00CF92000000FFFF            # 0x10: data
gdt64_ptr:
    .word gdt64_ptr - gdt64 - 1
    .quad gdt64

.section .text
# ---- Exception entry -------------------------------------------------------
# In long mode the CPU always pushes SS, RSP, RFLAGS, CS, RIP (and an error
# code for some vectors), and aligns RSP to 16 first. We push a dummy error
# code where the CPU didn't, the vector, and all 15 GPRs: struct frame.

.macro ISR_NOERR n
isr\n:
    push $0
    push $\n
    jmp isr_common
.endm

.macro ISR_ERR n
isr\n:
    push $\n
    jmp isr_common
.endm

ISR_NOERR 0
ISR_NOERR 1
ISR_NOERR 2
ISR_NOERR 3
ISR_NOERR 4
ISR_NOERR 5
ISR_NOERR 6
ISR_NOERR 7
ISR_ERR   8
ISR_NOERR 9
ISR_ERR   10
ISR_ERR   11
ISR_ERR   12
ISR_ERR   13
ISR_ERR   14
ISR_NOERR 15
ISR_NOERR 16
ISR_ERR   17
ISR_NOERR 18
ISR_NOERR 19
ISR_NOERR 20
ISR_ERR   21
ISR_NOERR 22
ISR_NOERR 23
ISR_NOERR 24
ISR_NOERR 25
ISR_NOERR 26
ISR_NOERR 27
ISR_NOERR 28
ISR_NOERR 29
ISR_NOERR 30
ISR_NOERR 31

# 5 CPU words + error + vector + 15 GPRs = 22 * 8 = 176 bytes, a multiple of
# 16, so RSP is 16-byte aligned at the call as the SysV ABI requires.
isr_common:
    push %rax
    push %rbx
    push %rcx
    push %rdx
    push %rsi
    push %rdi
    push %rbp
    push %r8
    push %r9
    push %r10
    push %r11
    push %r12
    push %r13
    push %r14
    push %r15
    mov %rsp, %rdi                      # argument: struct frame *
    call gdb_trap
    pop %r15
    pop %r14
    pop %r13
    pop %r12
    pop %r11
    pop %r10
    pop %r9
    pop %r8
    pop %rbp
    pop %rdi
    pop %rsi
    pop %rdx
    pop %rcx
    pop %rbx
    pop %rax
    add $16, %rsp                       # drop vector + error code
    iretq                               # pops RIP CS RFLAGS RSP SS - all editable

.section .data
.global isr_table
isr_table:
    .quad isr0, isr1, isr2, isr3, isr4, isr5, isr6, isr7
    .quad isr8, isr9, isr10, isr11, isr12, isr13, isr14, isr15
    .quad isr16, isr17, isr18, isr19, isr20, isr21, isr22, isr23
    .quad isr24, isr25, isr26, isr27, isr28, isr29, isr30, isr31
.section .text

# ---- Fault-safe memory access (as in Module 05) ------------------------------
.global safe_read_byte          # int safe_read_byte(u64 addr) -> 0..255 or -1
safe_read_byte:
.global safe_read_insn
safe_read_insn:
    movzbl (%rdi), %eax
    ret

.global safe_write_byte         # int safe_write_byte(u64 addr, u32 v) -> 0 or -1
safe_write_byte:
    mov %esi, %eax
.global safe_write_insn
safe_write_insn:
    movb %al, (%rdi)
    xor %eax, %eax
    ret

.global safe_fixup
safe_fixup:
    mov $-1, %eax
    ret

# ---- AP trampoline (Module 11) -----------------------------------------------
# A Startup IPI starts an AP in 16-bit real mode at vector * 0x1000. This
# code is copied to 0x8000 (vector 0x08) by smp_init(), so every address in
# it is computed as (label - ap_trampoline + 0x8000).
.set TRAMP, 0x8000
.code16
.global ap_trampoline
ap_trampoline:
    cli
    xor %ax, %ax
    mov %ax, %ds
    lgdtl (ap_gdt32_ptr - ap_trampoline + TRAMP)
    mov %cr0, %eax
    or $1, %eax                         # CR0.PE: protected mode
    mov %eax, %cr0
    ljmpl $0x08, $(ap_pm32 - ap_trampoline + TRAMP)
.code32
ap_pm32:
    mov $0x10, %ax
    mov %ax, %ds
    mov %ax, %ss
    mov $enter_long_mode, %eax          # absolute: a relative jmp would be off,
    jmp *%eax                           # since this code was copied to 0x8000
.align 8
ap_gdt32:
    .quad 0
    .quad 0x00CF9A000000FFFF            # 0x08: 32-bit code
    .quad 0x00CF92000000FFFF            # 0x10: data
ap_gdt32_ptr:
    .word ap_gdt32_ptr - ap_gdt32 - 1
    .long ap_gdt32 - ap_trampoline + TRAMP
.global ap_trampoline_end
ap_trampoline_end:

.code64
# Each AP takes the next stack slot atomically, then becomes a C function.
.global ap_entry
.type ap_entry, @function
ap_entry:
    mov $1, %eax
    lock xaddl %eax, ap_next_slot       # eax = this AP's slot (1, 2, ...)
    mov %eax, %edi
    mov %rdi, %rax
    inc %rax
    shl $14, %rax                       # (slot + 1) * 16384
    lea ap_stacks(%rax), %rsp
    xor %ebp, %ebp
    call gdb_ap_start                   # never returns
3:  hlt
    jmp 3b

.section .data
.global ap_booting
ap_booting:    .long 0
ap_next_slot:  .long 1
.section .text
