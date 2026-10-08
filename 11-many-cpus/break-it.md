# Break It: Many CPUs

## Challenge 1: Don't Stop the World

Make `stop_others()` return immediately, and run the check under `-smp 4`.

**Hint:** The "counters unchanged while stopped" assertion fails first. Then try reading
`ap_counter` twice in a row from the debugger: the values change between reads, so the
debugger's view of memory is a blur, not a snapshot. Now set a breakpoint on `ap_work`
and step. Another AP runs straight through the lifted `int3` during the step-over, and
the breakpoint silently stops working for it.

## Challenge 2: Use a Normal Interrupt Instead of NMI

Send a fixed-vector IPI (say vector 0x40, with an IDT entry for it) instead of NMI.

**Hint:** Every CPU in this kernel runs with interrupts disabled, so a fixed-vector
IPI sits pending forever and `stop_others()` waits forever. That's why stubs use NMI.
The opposite problem is real too: Linux's kgdb must cope with a CPU that's spinning with
interrupts off *and* is already inside an NMI handler. What would you do then?

## Challenge 3: Report the Stale Hit

Remove the `hit_bp && bp_find(f->rip) < 0` check, set a breakpoint on `ap_work`, hit it,
remove it and continue.

**Hint:** Under `-smp 4` you'll often get a second `T05thread:..;` immediately, for a
breakpoint you just deleted. If you also remove the early `rip -= 1`, so the rewind
happens after the lock as in Module 07, the CPU resumes at `ap_work+1` and skips
`push rbp`. `ap_work`'s `pop rbp` then takes the return address, and `ret` returns to
whatever word sits below it on the stack. The check catches the first symptom. Can you
reproduce the second?

## Challenge 4: Count CPUs With a Timeout

Replace `count_cpus()` with "wait 10 million `pause`s and see who checked in".

**Hint:** It passes locally. Then run it under heavy load (`stress`, or two
`verify.py` runs at once), or on a slow CI runner. Sometimes an AP checks in after the
BSP stopped counting, so `qfThreadInfo` lists three threads and that CPU is never
stopped. That's the reason the module asks the firmware instead.

## Challenge 5: Interrupt-Driven Break-in on Every CPU

Only the BSP polls the UART, so Ctrl-C always reports thread 1. Route COM1's IRQ (IRQ 4,
GSI 4 on QEMU's I/O APIC) to a vector, and enable interrupts on one CPU.

**Hint:** Now the stub isn't the only thing that can run while it "owns the machine":
the UART interrupt can fire inside `get_packet()`. Real stubs mask it, or run with IF=0,
while serving. And which CPU should receive it? Linux's kgdb lets any CPU take the
break-in and then stops the others, exactly like a breakpoint.

## Where to Go Next

- **[gdbstub-mcp](https://github.com/0xmortuex/gdbstub-mcp):** the client you served, and
  its BACKLOG.
- **[MCP Server — The Hard Way](https://github.com/0xmortuex/mcp-server-the-hard-way):** the
  other end of this wire, built from scratch.
- **[ACPI — The Hard Way](https://github.com/0xmortuex/acpi-the-hard-way):** the MADT that
  `count_cpus()` only skims.
- **[qemu-replay-mcp](https://github.com/0xmortuex/qemu-replay-mcp):** when a race between
  CPUs only happens once, record it and debug it backwards.
