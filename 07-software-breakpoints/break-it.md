# Break It: Breakpoints

## Challenge 1: No Rewind

Delete the `f->eip -= 1` line. Break on `add` and look at what gdbstub-mcp reports.

**Hint:** The stop is reported at `add+0x1`, which isn't an instruction boundary, so
the disassembly there is garbage (`89 e5` decodes differently out of context).
`describe_stop` won't match it to breakpoint #1, and the next `c` executes the second
half of an instruction.

## Challenge 2: No Step-Over

Make `resume` ignore breakpoints, so it always just returns. Break on `add`, then
continue.

**Hint:** The kernel re-executes the `int3` it's sitting on, traps instantly, and
reports the same breakpoint at the same place, forever. Every debugger has to solve this
somewhere. gdb solves it client-side: before resuming from a breakpoint it removes it,
steps, and reinserts it. gdbstub-mcp just sends `c`, so with this client the stub's
step-over is the only thing keeping the kernel moving.

## Challenge 3: Show the `0xCC`

Remove the shadowing in `m`. Break on `add`, continue, and run `python verify.py 09`.

**Hint:** Module 09's check asserts the stop shows `next: push ebp`, and now it says
`next: int3`. `debug_disassemble add` shows `int3` too. Now break on a function whose
second instruction is the one you care about, and step past a breakpoint in the middle
of it. Which reads would a prologue analyser make, and what would it conclude?

## Challenge 4: A Breakpoint in the Stub

Set `Z0` on `uart_getc`. Continue. What happens?

**Hint:** The next packet the stub tries to read hits the `int3`, re-enters `gdb_trap`
from inside `serve`, and nests forever. Real stubs refuse breakpoints inside their own
code range (`stub_start`..`stub_end`), and that's a good exercise: use linker symbols to
find the range.

## Challenge 5: Hardware Breakpoints

Implement `Z1` (hardware execute breakpoint) using DR0 and DR7. Remember that `#DB` now
has two causes, a single step and a debug-register hit, and DR6 tells you which.

**Hint:** Set DR0 to the address, and in DR7 set L0 (bit 0) with R/W0 = 00 (execute) and
LEN0 = 00. On `#DB`, read DR6: bit 0 means DR0 hit, and bit 14 (BS) means single step.
For an execute breakpoint, set the Resume Flag (EFLAGS bit 16) before `iret`, so it
doesn't fire again on the same instruction. Then `Z2` (write watchpoints) is almost
free.

→ [Next: Module 08](../08-telling-the-debugger-what-it-is-talking-to/README.md)
