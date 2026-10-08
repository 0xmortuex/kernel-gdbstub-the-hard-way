# Break It: Memory

## Challenge 1: The Naive Read

Replace `safe_read_byte(addr + i)` with `*(volatile u8 *)(addr + i)` and ask for
`m40000000,4`. Then ask for anything else.

**Hint:** The `#PF` re-enters `gdb_trap` through the normal path, and a second, nested
`serve()` starts waiting for packets. The `m` never gets its reply, so the client times
out. If it carries on, every register it reads now describes the stub's own fault
handler, not your kernel. This is exactly the failure the exception table exists to
prevent.

## Challenge 2: Check the Fixup Last

Move the `safe_read_insn` check to the end of `gdb_trap`, after the stop-reply logic.
Why does it matter where it goes?

**Hint:** Anything `gdb_trap` does before the check (sending a packet, changing
`last_signal`, touching breakpoints) happens for a fault that isn't a real stop. The
fixup must be the very first decision.

## Challenge 3: A Fault Inside the Fixup Window

`safe_read_byte` loads its argument into EDX *before* the guarded instruction. What if
the debugger could make `mov 4(%esp), %edx` fault?

**Hint:** It can't. ESP points into the stub's own stack, which is mapped. But that's an
argument, not an accident: keep the guarded window to exactly one instruction, and make
everything around it unable to fault. Linux's `__get_user` follows the same discipline.

## Challenge 4: Page-Straddling Reads

Read `m3ffffc,8`. How many bytes come back? Now read `m3ffff8,10` and count.

**Hint:** The mapping ends at `0x400000`. The stub returns the readable prefix. Make it
return `E14` instead whenever *any* byte faults, and see what gdbstub-mcp's
`debug_memory` does with a stack dump near the end of a mapping.

## Challenge 5: Read-Only Code

Make the page read-only (clear the writable bit, `0x81`) and set `CR0.WP`. What happens
to `M` on kernel code, and to Module 07's breakpoints?

**Hint:** With CR0.WP set, even ring 0 can't write read-only pages, so `M` returns
`E14`, and so would inserting an `int3`. Real stubs temporarily clear WP (or map a
writable alias of the page) to patch code. This is also how you'd catch a kernel
scribbling over its own `.text`.

→ [Next: Module 06](../06-continue-and-step/README.md)
