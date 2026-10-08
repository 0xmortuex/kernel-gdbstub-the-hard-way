# Module 04: Trapping and Registers

## The Concept

A stub only runs when the CPU takes an **exception**: a breakpoint (`int3`, vector 3), a
single-step trap (vector 1), a page fault (vector 14), and so on. The CPU stops what it's
doing, pushes a few registers, and jumps to the handler listed in the **Interrupt
Descriptor Table**. The stub's handler has two jobs:

1. **Save the interrupted CPU state** somewhere the debugger can read and edit it.
2. **Put it back** (edited or not) when the debugger says go.

Everything the debugger calls "the registers" is really the saved copy in memory. When
you `set $eax = 5` in gdb, the stub writes 5 into that saved copy, and the CPU only sees
it when the handler returns.

## The IDT

```c
void gdb_init(void) {
    u16 cs;
    __asm__ volatile("mov %%cs, %0" : "=r"(cs));   /* reuse the loader's code segment */
    for (int i = 0; i < 32; i++) {
        u32 h = isr_table[i];
        idt[i].off_lo = (u16)(h & 0xFFFF);
        idt[i].selector = cs;
        idt[i].zero = 0;
        idt[i].type_attr = 0x8E;   /* present, DPL0, 32-bit interrupt gate */
        idt[i].off_hi = (u16)(h >> 16);
    }
    struct idtr r;
    r.limit = sizeof(idt) - 1;
    r.base = (u32)idt;
    __asm__ volatile("lidt %0" : : "m"(r));
}
```

All 32 CPU exception vectors point at the stub. Then *any* crash (`#UD`, `#GP`, `#PF`,
divide by zero) stops in the debugger instead of triple-faulting. That alone makes a stub
worth having.

`0x8E` is an **interrupt gate**: on entry the CPU clears IF, so no interrupt can preempt
the stub, and clears TF, so a single step doesn't trap inside the handler itself. The
selector is whatever CS the loader gave us. The course doesn't build its own GDT, and
reading CS avoids assuming one.

## One Frame Layout for Every Vector

Some exceptions push an error code (8, 10–14, 17, 21) and the rest don't. If the handler
had to know which, every path would be a special case. So each vector gets a two-line
stub in `boot.s` that makes them all look the same:

```asm
.macro ISR_NOERR n          .macro ISR_ERR n
isr\n:                      isr\n:
    push $0                     push $\n
    push $\n                    jmp isr_common
    jmp isr_common          .endm
.endm
```

Then one common path saves everything else and calls C:

```asm
isr_common:
    pusha                   # eax ecx edx ebx esp ebp esi edi
    push %ds
    push %es
    push %fs
    push %gs
    push %esp               # argument: struct frame *
    call gdb_trap
    add $4, %esp
    pop %gs ... pop %ds
    popa
    add $8, %esp            # drop vector + error code
    iret
```

Read the stack from the lowest address up, and you have the C struct:

```c
struct frame {
    u32 gs, fs, es, ds;                                    /* pushed by us    */
    u32 edi, esi, ebp, esp_at_pusha, ebx, edx, ecx, eax;   /* pusha           */
    u32 vector, err;                                       /* stub + CPU      */
    u32 eip, cs, eflags;                                   /* pushed by CPU   */
};
```

`gdb_trap(struct frame *f)` receives a pointer to the live saved state. Writing `f->eax`
changes what `popa` restores. Writing `f->eip` changes where `iret` returns. That's the
whole mechanism behind "set a register".

## The ESP Problem

The `esp` that `pusha` saved is *inside the handler*, not the interrupted code's. For a
ring-0 trap, the CPU doesn't push SS:ESP, because there's no stack switch. So the
interrupted ESP is just the address above the three words the CPU pushed:

```c
static u32 trapped_esp(struct frame *f) { return (u32)&f->eflags + 4; }
```

That's also why the stub **refuses to write ESP**. The frame lives at the old stack
pointer, and `iret` pops EIP/CS/EFLAGS from there. Moving ESP would mean moving the frame,
which this stub doesn't do. Segment registers aren't writable either: a bad selector
would fault on the way out of the handler.

## Registers on the Wire

gdb's i386 register order is fixed: `eax ecx edx ebx esp ebp esi edi eip eflags cs ss ds
es fs gs`, 16 registers of 4 bytes, each sent as **little-endian hex**. `g` sends all of
them, `G` writes all of them, `p n` reads one and `P n=v` writes one:

```c
case 'g':
    for (int i = 0; i < NUM_REGS; i++)
        o = put_hex_u32_le(o, reg_get(f, i));
```

`put_hex_u32_le(0x0010534c)` produces `4c531000`: the bytes in memory order. Getting
this backwards is the most common stub bug there is. The debugger shows EIP as
`0x4c531000`, every backtrace is nonsense, and nothing crashes, so it's easy to miss.

## The Boot-Time Breakpoint

The kernel calls a compiled-in breakpoint right after setting up the IDT:

```c
void gdb_breakpoint(void) { __asm__ volatile("int3"); }
```

`int3` is a one-byte instruction (`0xCC`) that raises vector 3. The kernel now sits in
`serve()`, answering packets, before it does anything else. That's the in-kernel version
of QEMU's `-S`: halt at the start, so the debugger can set things up.

## The Check

```
ok  handshake: qSupported -> {'PacketSize': '1000'}
ok  '?' -> S05 (SIGTRAP: stopped at the boot-time int3)
ok  'g' -> 16 registers; eip = gdb_breakpoint+0x4 (just after the int3)
ok  esp = 0x10534c is inside the kernel stack (rebuilt from the frame address)
ok  'G' wrote eax=0x12345678 into the trap frame; 'g' reads it back
ok  'p 8' returns eip; 'P 3=...' sets ebx
ok  'P' on esp is refused with E01 (the frame lives on that stack)
```

From here on, `check.py` uses gdbstub-mcp's `RSPClient`, the real client the agent
tools use, rather than raw bytes.

Note **eip = gdb_breakpoint+0x4**. `int3` is a *trap*: EIP points after the instruction
that caused it. For a compiled-in breakpoint that's what you want, since continuing
moves on. For a breakpoint the *debugger* inserted, it's wrong, and Module 07 fixes it.

## What This Stub Can't Do Yet

It can't continue. `c` gets an empty reply, so the kernel is parked at the boot
breakpoint forever. Module 05 adds memory, and Module 06 adds the way out.
