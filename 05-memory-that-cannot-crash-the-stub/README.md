# Module 05: Memory That Can't Crash the Stub

## The Concept

`m` reads memory and `M` writes it:

```
$m101380,4#..          read 4 bytes at 0x101380   ->  $5589e58b#..
$M10c1b8,4:efbeadde#.. write 4 bytes              ->  $OK#9a
```

The naive implementation is one line: `*(u8 *)addr`. It works until the debugger asks
for an address that isn't mapped, which debuggers do constantly. A backtrace follows a
corrupt frame pointer. A `print *p` dereferences garbage. gdbstub-mcp's stack scan
reads 4 KiB above the stack pointer without knowing where the stack ends.

When a kernel reads unmapped memory, the CPU raises a page fault. The page fault handler
is the stub. The stub is already in the middle of handling a breakpoint, so it now
re-enters itself with a half-written reply, and the machine dies inside the debugger.
The one tool you were using to understand a crash just caused one.

The rule: **every memory access the stub makes on the debugger's behalf must be allowed
to fail, and failing must produce an answer (`E14`), not a crash.**

## Making Faults Possible

First, a fault has to be possible. With paging off, every 32-bit address "exists", and
reading past RAM just returns `0xff`. So this module turns paging on with the smallest
possible setup: one 4 MiB page that identity-maps the first 4 MiB, where the kernel
lives, and nothing else:

```c
static u32 page_dir[1024] __attribute__((aligned(4096)));

static void paging_init(void) {
    page_dir[0] = 0x00000083;                   /* present | writable | 4 MiB */
    ...  CR4 |= PSE;  CR3 = page_dir;  CR0 |= PG;
}
```

Now `0x40000000` is genuinely unmapped, and touching it raises `#PF`, just like a wild
pointer in a real kernel.

## The Exception Table

The fix is a technique Linux calls an **exception table**: before an access that might
fault, record *exactly which instruction* is allowed to fault and where to go if it
does. The access lives in assembly so its address is known:

```asm
.global safe_read_byte          # int safe_read_byte(u32 addr) -> 0..255 or -1
safe_read_byte:
    mov 4(%esp), %edx
.global safe_read_insn
safe_read_insn:
    movzbl (%edx), %eax         # <- the only instruction allowed to fault
    ret

.global safe_fixup
safe_fixup:
    mov $-1, %eax               # "it faulted"
    ret
```

The first thing `gdb_trap` does is check whether this fault is one of those:

```c
if ((f->vector == 13 || f->vector == 14) &&
    (f->eip == (u32)safe_read_insn || f->eip == (u32)safe_write_insn)) {
    f->eip = (u32)safe_fixup;
    return;
}
```

Follow it through. The stub is in `serve()` and calls `safe_read_byte(0x40000000)`.
`movzbl` faults, the CPU enters `isr14`, and `gdb_trap` sees EIP is `safe_read_insn`. It
rewrites the saved EIP to `safe_fixup` and returns. `iret` lands in `safe_fixup`, which
sets EAX to -1 and executes `ret`. Because `safe_read_byte` never pushed anything, that
`ret` goes straight back to `serve()`, which sees -1.

```
 serve() ─call─► safe_read_byte ─► movzbl (0x40000000) ──#PF──► gdb_trap
    ▲                                                            │ eip == safe_read_insn
    │                                                            ▼ eip = safe_fixup
    └────────────────────── ret ◄── safe_fixup: eax = -1 ◄── iret
```

The nested exception never reaches the "report a stop to the debugger" path. Only faults
at those exact two addresses are forgiven. A fault anywhere else in the stub is still a
real bug, and still stops in the debugger, which is what you want.

This is also why the check happens *first* in `gdb_trap`, before anything that would
send a packet.

## `m` and `M`

```c
for (i = 0; i < len; i++) {
    int b = safe_read_byte(addr + i);
    if (b < 0) break;                      /* faulted: stop here */
    o = put_hex_byte(o, (u8)b);
}
put_packet(i == 0 ? "E14" : outbuf);       /* E14 = EFAULT */
```

Byte by byte, stopping at the first fault. If nothing was readable, the reply is `E14`
(errno 14 is EFAULT). If *some* bytes were readable, the stub returns those. The
protocol allows short reads, and a read that straddles the end of a mapping is common
when a debugger dumps a stack.

`M` is the same with `safe_write_byte`, and returns `OK` or `E14`.

The length is capped at what `outbuf` can hold. gdbstub-mcp reads in 0x800-byte chunks,
which comes to 4096 hex characters per reply, so `outbuf` is 8 KiB. A stub with a
smaller buffer would advertise a smaller `PacketSize` in `qSupported` (Module 08), and a
well-behaved client respects it.

## The Check

```
ok  'm' at kmain returns exactly the bytes in the ELF's .text
ok  4 KiB read as two 0x800-byte packets (4096 hex chars per reply)
ok  'M' wrote counter = 0xdeadbeef and 'm' reads it back
ok  'm' at 0x40000000 (unmapped: paging maps only 0-4 MiB) -> E14, no crash
ok  the stub is still alive after the #PF it caught
ok  a read straddling the end of the mapping returns the 8 readable bytes
ok  'M' at an unmapped address -> E14 as well
```

The first line compares live memory with the bytes in the ELF file. That's how you know
both that the read path works and that the kernel was loaded where the linker said.

## Byte at a Time Is Deliberate

Reading 4 bytes as one `u32` is faster. It's also wrong for a debugger. A device
register might have side effects per access width, and an unaligned word read can fault
on the second page when the first byte is fine. Byte accesses are slow, predictable and
exact. At 115200 baud the serial line is a thousand times slower than any of this
anyway.
