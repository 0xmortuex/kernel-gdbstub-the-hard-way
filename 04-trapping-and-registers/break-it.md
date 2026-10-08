# Break It: Traps and Frames

## Challenge 1: Swap Two Pushes

In `isr_common`, push `%es` before `%ds` but leave `struct frame` alone. Run the check.
Which assertion fails? Which register is now wrong without any assertion noticing?

**Hint:** `ds` and `es` hold the same selector in this kernel, so swapping them changes
nothing visible. That's the dangerous kind of frame bug: it only shows up the day the
two differ. Try swapping `pusha` with the segment pushes instead and watch every
register shift.

## Challenge 2: Big-Endian Registers

Change `put_hex_u32_le` to emit the most significant byte first. What does gdbstub-mcp
show for EIP, and does anything *crash*?

**Hint:** EIP becomes something like `0xc4031000`. Nothing faults: the debugger simply
believes the wrong number, symbolizes it to nothing, and reads memory from the wrong
place. Endianness bugs in a stub are silent.

## Challenge 3: Write ESP Anyway

Make `reg_set` accept register 4 by writing `f->esp_at_pusha`. Set ESP with `P4=...` and
continue (after Module 06). What happens?

**Hint:** `popa` *skips* the saved ESP slot. It's documented as discarded. So nothing
changes. To truly move the stack, you'd copy the iret frame to the new ESP and switch to
it before `iret`. That's possible, and it's how stubs that support stack switching do
it.

## Challenge 4: Missing Error Code

Mark vector 14 (`#PF`) as `ISR_NOERR` instead of `ISR_ERR`. Trigger a page fault
(Module 05 makes that easy) and inspect EIP.

**Hint:** The CPU pushed an error code, and the stub pushed a dummy one too. Everything
above is now off by one word, so "EIP" is really the CPU's error code, and `iret` returns
to an address that is actually the code segment. The machine triple-faults inside the
debugger. The vector table in `boot.s` is not decoration.

## Challenge 5: A Trap Gate

Change `type_attr` from `0x8E` (interrupt gate) to `0x8F` (trap gate). Which behaviour
changes?

**Hint:** Trap gates don't clear IF. With interrupts disabled kernel-wide, nothing
changes yet. But once Module 09 enables break-in, or the kernel turns interrupts on, a
timer interrupt could fire inside the stub, while it's halfway through a packet.

→ [Next: Module 05](../05-memory-that-cannot-crash-the-stub/README.md)
