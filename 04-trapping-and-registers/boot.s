# boot.s - multiboot entry for the course kernel.
# QEMU's -kernel loader (or GRUB) jumps to _start in 32-bit protected mode
# with flat segments, paging off and interrupts disabled.

.set MAGIC, 0x1BADB002
.set FLAGS, 0x0
.section .multiboot
.align 4
.long MAGIC
.long FLAGS
.long -(MAGIC + FLAGS)

.section .bss
.align 16
stack_bottom:
.skip 16384
stack_top:

.section .text
.global _start
.type _start, @function
_start:
    cli
    mov $stack_top, %esp
    call kmain
1:  hlt
    jmp 1b

# ---- Exception entry (Module 04) -------------------------------------------
# Every vector gets a tiny stub that makes the stack look the same:
# the CPU pushed EFLAGS, CS, EIP (and an error code for some vectors); we push
# a dummy error code where the CPU didn't, then the vector number, so the
# common path below sees one fixed layout: struct frame in stub.c.

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

# Save everything, hand C a pointer to it, restore everything (possibly
# modified by the debugger), and return to wherever the CPU was.
isr_common:
    pusha                   # eax ecx edx ebx esp ebp esi edi
    push %ds
    push %es
    push %fs
    push %gs
    push %esp               # argument: struct frame *
    call gdb_trap
    add $4, %esp
    pop %gs
    pop %fs
    pop %es
    pop %ds
    popa
    add $8, %esp            # drop vector + error code
    iret

.section .data
.global isr_table
isr_table:
    .long isr0, isr1, isr2, isr3, isr4, isr5, isr6, isr7
    .long isr8, isr9, isr10, isr11, isr12, isr13, isr14, isr15
    .long isr16, isr17, isr18, isr19, isr20, isr21, isr22, isr23
    .long isr24, isr25, isr26, isr27, isr28, isr29, isr30, isr31
.section .text
