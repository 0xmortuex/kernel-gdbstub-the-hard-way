# Break It: The Whole Stub

## Challenge 1: An Infinite Loop Elsewhere

Add `for (;;) {}` inside `compute`, and try to interrupt the kernel with
`debug_interrupt`.

**Hint:** `gdb_poll` never runs, so the `0x03` sits in the UART forever and the client
reports that the target didn't stop. This is the limit of polled break-in. Fix it with
the UART's receive interrupt: unmask IRQ 4 on the PIC (after remapping it away from the
CPU exception vectors 0–31), set IER bit 0, and execute `int3` from the IRQ handler.

## Challenge 2: A Crash, Caught

Make `add` dereference a NULL pointer plus 0x40000000, so it's outside the 4 MiB
mapping. Continue with gdbstub-mcp. What does it report?

**Hint:** The `#PF` lands in `gdb_trap` like any other exception. It isn't a
`safe_read_insn` fault, so the stub reports `S0b` (SIGSEGV). `debug_backtrace` shows
how you got there, and `debug_registers` shows the bad pointer. A kernel that used to
triple-fault now stops in a debugger, and that alone justifies the whole course.
Bonus: add CR2 to `target.xml` (Module 08, Challenge 2) so the agent can see the
faulting address.

## Challenge 3: Two Debuggers

Connect gdbstub-mcp, disconnect, and connect real gdb (`target remote localhost:PORT`).
What does gdb send that gdbstub-mcp didn't, and does the stub cope?

**Hint:** gdb sends `vCont?`, `qTStatus`, `Hg0`, `qfThreadInfo`, `qAttached` and more.
Each gets an empty reply ("not supported"), and gdb falls back. `H` packets select a
thread: a stub with one thread can answer `OK`. Log every packet the stub receives to
see a real client's opening conversation.

## Challenge 4: Watchpoints Through Debug Registers

With Module 07's Challenge 5 done (DR0–DR3), implement `Z2` (write watchpoint) and use
`debug_break("hw", "counter", kind="write")`. Where does it stop?

**Hint:** Data breakpoints trap *after* the write, so EIP is the instruction after the
store. The stop reply should carry `watch:<addr>;`, which gdbstub-mcp reports as
`write hit on counter`. Use the `T` stop-reply form, `T05watch:10c1b8;`.

## Challenge 5: x86-64

Port the stub to long mode. What changes?

**Hint:** The frame grows (16 general registers, `iretq`, and the CPU now always pushes
SS:RSP, so the ESP problem from Module 04 goes away). Registers are 8 bytes in `g`. The
XML becomes `i386:x86-64` with `org.gnu.gdb.i386.core` listing `rax`…`r15`, `rip`,
`eflags` and the segment registers. Everything above the frame (framing, memory,
breakpoints, qXfer) is unchanged. That's the payoff of keeping the layers separate.
Module 10 does exactly this port.

→ [Next: Module 10](../10-long-mode/README.md)
