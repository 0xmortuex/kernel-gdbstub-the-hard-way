# Break It: Running and Stepping

## Challenge 1: Forget to Clear TF

Delete `f->eflags &= ~EFLAGS_TF;` in `gdb_trap`. Step once, then continue. What does the
debugger see?

**Hint:** The continue stops after one instruction, because `iret` restored TF=1. The
debugger thinks it asked for "run" and got "stopped by SIGTRAP" at the next
instruction, every time. The check's TF assertion exists to catch this.

## Challenge 2: Talk When Nobody Asked

Remove the `if (debugger_waiting)` guard so every trap sends `S05`. Run the Module 04
check.

**Hint:** The boot breakpoint fires while the client's `qSupported` is in flight. The
client reads `S05` as the `qSupported` reply, then every later reply is one packet out
of step. gdbstub-mcp's handshake fails. Unsolicited packets are the most common way a
homemade stub breaks a real client.

## Challenge 3: Step Over `iret`

Put a breakpoint on an `iret` instruction (say, in a future interrupt handler) and step.
What happens to TF?

**Hint:** `iret` loads EFLAGS from the stack. If the popped value has TF clear, the step
trap after `iret` may never fire, so the debugger waits forever for a stop that isn't
coming. Instructions that load EFLAGS (`popf`, `iret`) need special handling in serious
stubs.

## Challenge 4: Step Into the Stub

Set TF and step while the kernel is about to call `gdb_poll()` (Module 09). Where does
the next stop land?

**Hint:** Inside the stub's own polling code. Interrupt gates only clear TF on *entry to
an exception handler*. A normal call into stub code is just kernel code to the CPU. Real
stubs refuse to step into their own text, or mark it "no-step".

## Challenge 5: A Detach That Leaves Breakpoints

Detach (`D`) while Module 07's breakpoints are still inserted. Then reconnect.

**Hint:** The `int3` bytes are still in memory, so the kernel traps on its own and waits
for a debugger that isn't there. gdbstub-mcp removes its breakpoints before sending `D`.
A robust stub also clears them all itself on detach.

→ [Next: Module 07](../07-software-breakpoints/README.md)
