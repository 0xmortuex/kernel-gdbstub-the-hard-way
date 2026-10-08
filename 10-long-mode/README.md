# Module 10: Long Mode

## The Concept

Everything so far ran in 32-bit protected mode. Real kernels today are 64-bit, and the
stub has to come along. Surprisingly little of it changes: the UART, the framing, the
breakpoints and the exception table all carry over. What changes is everything that
touches the CPU's shape:

- how the kernel gets into long mode at all
- the trap frame the CPU builds
- the IDT gate format
- the register list in `target.xml` and the `g` packet

This module's kernel lives in [`src64/`](../src64/), staged the same way as `src/`.

## Getting Into Long Mode

QEMU's `-kernel` loader, like GRUB's multiboot, starts the CPU in 32-bit protected mode
with paging off. Worse, QEMU refuses to load an ELF64 multiboot kernel at all. Two
tricks get around that:

1. **The a.out kludge.** Bit 16 of the multiboot flags says "the load addresses are in
   the header". The loader then copies the file to `load_addr` without parsing it, so
   `build.py` boots a flat binary (`objcopy -O binary`). The ELF64 is still what the
   debugger reads symbols from.
2. **A 32-bit trampoline.** `_start` is `.code32`. It builds page tables, turns long
   mode on, and far-jumps into 64-bit code:

```
 _start (.code32)                      long_start (.code64)
   PML4[0] → PDPT[0] → PD: 512 × 2 MiB    mov $0x10 → ds es ss fs gs
   CR4.PAE = 1                            mov $stack_top, %rsp
   CR3 = pml4                             xor %ebp, %ebp     ← end of backtraces
   EFER.LME = 1  (MSR 0xC0000080)         call kmain
   CR0.PG = 1    → long mode active
   lgdt gdt64; ljmp $0x08, $long_start   (0x08: a code descriptor with L=1)
```

Long mode requires paging, so the identity map isn't optional any more. It covers the
first GiB and nothing else, which keeps Module 05's lesson intact: `0x40000000` is the
first unmapped byte, and a read there must answer `E14`.

Two details matter. After the switch, the upper half of RSP is undefined, so
`long_start` loads a full 64-bit stack pointer instead of trusting the 32-bit one. And
`long_start` is exported as a function symbol. Without that, the linker drops the local
label, and the last frame of every backtrace shows as `_start+0x96`.

## The 64-bit Trap Frame

In long mode the CPU pushes **SS and RSP on every exception**, even ring 0 to ring 0,
and it aligns RSP to 16 bytes first:

```
 high   ss
        rsp        ← the interrupted code's stack pointer, now IN the frame
        rflags
        cs
        rip
        err        (CPU or our dummy 0)
        vector     (ours)
        rax rbx rcx rdx rsi rdi rbp r8 … r15   (isr_common)
 low    ← struct frame *
```

That's 22 words, or 176 bytes, a multiple of 16, so RSP is aligned when `isr_common`
calls into C, as the SysV ABI requires. It returns with `iretq`, which pops RIP, CS,
RFLAGS, **RSP** and SS.

That has a consequence for the debugger. In Module 04, `P` on ESP had to answer `E01`,
because the 32-bit frame *is* the old stack pointer and moving it would make `iret`
pop garbage. Here RSP is just another slot, and `iretq` loads it. So RSP becomes
writable, and `check.py` writes it and reads it back.

Interrupt gates grow to 16 bytes, with the handler address split three ways
(`off_lo`, `off_mid`, `off_hi`) and an IST field. This kernel leaves the IST at 0, so
every exception runs on the current stack. That's why `build.py` compiles with
**`-mno-red-zone`**: the SysV ABI lets leaf functions use 128 bytes *below* RSP without
moving it, and an exception frame pushed there would overwrite them. Every 64-bit
kernel turns the red zone off.

## Registers in gdb's Order

The amd64 numbering is not the hardware encoding order. It's gdb's, and a client that
reads `target.xml` depends on it:

```
  0 rax  1 rbx  2 rcx  3 rdx  4 rsi  5 rdi  6 rbp  7 rsp   8-15 r8-r15
 16 rip  17 eflags  18 cs 19 ss 20 ds 21 es 22 fs 23 gs   24 fs_base  25 gs_base
```

Sizes vary: the GPRs and `rip` are 8 bytes, `eflags` and the selectors 4, and the two
bases 8. So `g` is 17×8 + 7×4 + 2×8 = **180 bytes**, and the encoder now takes a size:

```c
for (int i = 0; i < NUM_REGS; i++)
    o = put_hex_le(o, reg_get(sel, i), reg_size(i));
```

`fs_base` and `gs_base` aren't in the frame at all. They're MSRs (`0xC0000100` and
`0xC0000101`), read with `rdmsr`. The architecture string becomes `i386:x86-64`, which
is how gdbstub-mcp knows to disassemble in 64-bit mode.

## Non-Canonical Addresses

A 64-bit address space adds a new way for a read to fail. Only 48 bits of an address are
real, and bits 63..47 must all be copies of bit 47. Touching an address that breaks
this rule is a **#GP**, not a #PF. The exception table from Module 05 already catches
both vectors, so `m8000000000000000,4` answers `E14` like any unmapped read. `check.py`
proves it.

## Same Client, New CPU

The second half of `check.py` points gdbstub-mcp at the stub and changes nothing:

```
Connected 'hw64' (i386:x86-64, 26 registers).
Stopped: SIGTRAP (breakpoint/step) - breakpoint #1 (add)
pc = 0x101bd0 <add> at kernel.c:12
next: push rbp
#0  0x101bd0 <add> at kernel.c:12
#1  0x101c1c <compute+0x2c> at kernel.c:19
#2  0x101c5d <kmain+0x1d> at kernel.c:28
#3  0x1000b6 <long_start+0x1c> at boot.s:91
```

That's Module 08's argument made concrete. The client learned a whole new register file
from the stub's own description, and its backtrace code walked 8-byte frame pointers
because `rbp` was declared 64 bits wide.
