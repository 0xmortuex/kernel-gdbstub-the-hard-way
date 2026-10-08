# Module 06: Continue and Step

## The Concept

Until now the stub could look but not let go: the kernel was parked at its boot
breakpoint forever. This module adds the two ways out:

- **`c` (continue):** return from the exception and let the kernel run until something
  traps again.
- **`s` (step):** run exactly *one* instruction, then trap again.

Continuing is just *returning*. `serve()` returns to `gdb_trap`, which returns to
`isr_common`, which restores the (possibly edited) frame and executes `iret`. Stepping
needs help from the CPU.

## The Trap Flag

Bit 8 of EFLAGS is **TF**, the trap flag. When it's set, the CPU raises a debug
exception (`#DB`, vector 1) after executing one instruction. So stepping is:

```c
static void resume(struct frame *f, int step) {
    if (step) f->eflags |= EFLAGS_TF;
    debugger_waiting = 1;
}
```

Set TF in the *saved* EFLAGS. When `iret` restores EFLAGS, TF takes effect. The CPU runs
one instruction of the kernel and traps into vector 1, back into `gdb_trap`.

Two details make this work. Because the IDT uses interrupt gates, the CPU clears TF on
entry to the handler, so the stub itself doesn't single-step through its own code. And
the stub must clear TF in the saved frame before reporting the stop:

```c
f->eflags &= ~EFLAGS_TF;                    /* a single step is over */
```

Otherwise the *next* `c` would also step, because `iret` would restore TF=1, and
"continue" would silently behave like "step".

## When to Send a Stop Reply

`c` and `s` are the only packets without an immediate reply. The debugger sends `c`,
and the answer is the *next stop*, which may come a microsecond later, an hour later, or
never. So the stub has to remember that someone is waiting:

```c
if (debugger_waiting) {                     /* answer the pending c/s */
    ...
    debugger_waiting = 0;
    put_packet(reply);                      /* "S05" */
}
serve(f);
```

Why not send a stop reply on *every* trap? Because some traps happen when nobody asked.
The boot-time `int3` fires the moment QEMU starts the CPU, which is when the debugger
connects. At that moment the debugger is sending `qSupported`, not waiting for a stop.
An unsolicited `S05` would be read as the reply to `qSupported` and desynchronise the
whole session (Challenge 2 shows it happening). A stub must only speak when spoken to,
with one exception: the reply to `c`/`s`.

The debugger learns the initial stop the polite way, by asking `?`.

## Getting Back In

Once `c` lets the kernel run, how does it ever stop again? There are no breakpoints yet
(Module 07) and no Ctrl-C (Module 09). This module uses the oldest trick there is, a
**compiled-in breakpoint** the debugger can arm:

```c
volatile unsigned int break_now;   /* the debugger writes 1 here to stop in kmain */

for (;;) {
    counter += (unsigned int)compute(5);
    if (break_now) {
        break_now = 0;
        gdb_breakpoint();
    }
}
```

The debugger writes `1` to `break_now` with `M`, then sends `c`. The kernel runs a loop
iteration, sees the flag, and executes `int3`. Linux's `kgdb_breakpoint()` and the
`sysrq-g` key work the same way: the kernel calls into the debugger at a moment of its
choosing.

## `D`: Detach

`D` means "I'm leaving, let the machine run". The stub replies `OK`, resumes like `c`,
and clears `debugger_waiting`, because nobody will be there to read a stop reply. If the
kernel traps later, the stub serves packets, waiting for a debugger to reconnect.

## The Check

```
ok  6 single steps: gdb_breakpoint+0x4 -> gdb_breakpoint+0x5 -> kmain+0x13
                    -> kmain+0x1a -> compute -> compute+0x1 -> compute+0x3
ok  'c' ran the kernel (counter 0 -> 10) until the break_now int3 stopped it
ok  'c' again: no stop reply while the kernel runs
```

Read the step trace. `gdb_breakpoint+0x4` is right after the `int3`, at the `pop ebp`.
Stepping it reaches `+0x5`, the `ret`, and stepping that lands back in `kmain`. `kmain+0x13` to `+0x1a` is
the call setup for `compute(5)`, and then the steps enter `compute` and walk its
prologue. Each step is one machine instruction, and the trace proves it: no instruction
was skipped, and none repeated.

The check also asserts TF is clear after every reported step. That's the bug from the
previous section, caught before it can turn a later `c` into a step.

The last line: after the second `c`, nothing comes back for half a second, and that's
correct. The kernel is running. In this module there's no way to stop it again from
outside, so the check ends there. Module 07 adds breakpoints and Module 09 adds Ctrl-C.

## `c addr` and `vCont`

The protocol lets `c` and `s` carry an address to resume at, and the richer `vCont`
packet handles per-thread actions. This stub ignores both: an address can be set with
`P8=...` first, and a single-CPU kernel has one thread. A stub that doesn't support
`vCont` answers `vCont?` with an empty reply, and clients fall back to plain `c`/`s`.
