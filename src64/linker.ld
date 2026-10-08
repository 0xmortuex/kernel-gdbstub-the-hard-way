ENTRY(_start)
SECTIONS {
    . = 1M;
    __load_start = .;
    .text : { *(.multiboot) *(.text*) }
    .rodata : { *(.rodata*) }
    .data : { *(.data*) }
    __load_end = .;
    .bss : { *(COMMON) *(.bss*) }
    __bss_end = .;
}
