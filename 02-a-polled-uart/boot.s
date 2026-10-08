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
