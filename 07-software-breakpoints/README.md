# Module 07: Software Breakpoints

## The Concept

QEMU's stub stops at a breakpoint by watching the emulated CPU from outside. A stub
running *on* the CPU can't do that. So it does the oldest trick in debugging: it
**overwrites the first byte of the target instruction with `int3`** (`0xCC`). When
execution reaches it, the CPU traps into the stub.

```
 add:  55 89 e5 8b ...        push ebp / mov ebp,esp / ...
 Z0:   cc 89 e5 8b ...        int3     / mov ebp,esp / ...   (0x55 saved)
```

`int3` is exactly one byte, so it fits over the first byte of *any* x86 instruction. It
was designed for this.

Inserting is easy. Making the debugger never notice you did it takes three more pieces:
**rewind** EIP on a hit, **step over** the breakpoint when continuing, and **hide** the
`0xCC` from memory reads.

## Inserting and Removing

```c
static int bp_insert(u32 addr) {
    ...
    int orig = safe_read_byte(addr);
    if (orig < 0) return -1;
    if (safe_write_byte(addr, 0xCC) < 0) return -1;   /* int3 */
    bps[i].addr = addr;
    bps[i].orig = (u8)orig;
    bps[i].active = 1;
```

`Z0,addr,kind` inserts and `z0,addr,kind` removes. The `0` means "software breakpoint".
Types 1–4 (hardware breakpoints and watchpoints) get an empty reply, meaning not
supported, and a client falls back or reports it. The accesses go through Module 05's
fault-safe helpers, so `Z0` on a bogus address answers `E01` instead of crashing.

## 1. Rewind on a Hit

`int3` is a trap, so EIP points *after* it, at `add+1`. The debugger asked to stop *at*
`add`, and `add+1` is the middle of an instruction. So when the trap is one of ours,
step EIP back:

```c
if (f->vector == 3 && bp_find(f->eip - 1) >= 0)
    f->eip -= 1;
```

Only for our own breakpoints. The compiled-in `gdb_breakpoint()` `int3` isn't in the
table, and for it, "after the instruction" is exactly where execution should continue.

## 2. Step Over When Continuing

Now EIP points at a `0xCC`. If the stub just returns on `c`, the CPU executes the `int3`
again immediately: the kernel never gets past the breakpoint. The fix is the classic
**step-over dance**:

```
 c at a breakpoint:
   1. put the original byte back       add: 55 ...
   2. set TF, iret                     CPU runs `push ebp`, then #DB
   3. in #DB: write 0xCC again         add: cc ...
   4. clear TF, iret                   kernel continues at full speed
```

```c
static void resume(struct frame *f, int step) {
    int i = bp_find(f->eip);
    step_requested = step;
    if (i >= 0) {
        safe_write_byte(f->eip, bps[i].orig);
        stepping_over = f->eip;
        f->eflags |= EFLAGS_TF;
    } else if (step) {
        f->eflags |= EFLAGS_TF;
    }
    debugger_waiting = 1;
}
```

And in `gdb_trap`, the `#DB` that follows:

```c
if (f->vector == 1 && stepping_over) {
    safe_write_byte(stepping_over, 0xCC);   /* put the breakpoint back */
    stepping_over = 0;
    if (!step_requested) {                  /* that was our step: keep going */
        f->eflags &= ~EFLAGS_TF;
        return;
    }
}
```

`step_requested` distinguishes the two cases that look identical to the CPU. For `c`,
the single step was the stub's private business: re-arm and return without telling
anyone. For `s`, the user asked for exactly one instruction, so re-arm and report the
stop.

## 3. Hide the Patches

The debugger reads memory constantly: to disassemble at the stop address, to analyse a
function prologue during a backtrace, to show you the code. If `m` returned `0xCC`,
every stop at a breakpoint would report `next: int3`, and every disassembly would show
your patches instead of the program. gdb keeps its own shadow copies of the bytes it
replaced, but simpler clients like gdbstub-mcp trust whatever `m` returns, so a stub
that hides its patches works with both.

So reads substitute the saved byte:

```c
int k = bp_find(addr + i);             /* hide our own int3s */
if (k >= 0) b = bps[k].orig;
```

and writes over a breakpoint update the saved byte instead of clobbering the `int3`:

```c
int k = bp_find(addr + i);             /* keep the int3, update what it hides */
if (k >= 0) { bps[k].orig = b; continue; }
```

## The Check

```
ok  Z0 at add (0x101380); 'm' still shows the original bytes 5589e58b (int3 hidden)
ok  hit: eip rewound to exactly add (not add+1); b = 0
ok  continued from the breakpoint and hit it on the next call: b = 1
ok  's' from the breakpoint executed add's first instruction -> add+0x1
ok  the breakpoint was re-armed after the step (hit again)
ok  z0 removed it; break_now=1 brought us back via the compiled-in breakpoint
```

The second and third lines are the step-over dance proving itself. `compute` calls
`add(total, i)` for i = 0..4. The check reads the argument `b` off the stack at each
hit: `[esp+8]`, because at the first instruction `[esp]` is the return address and
`[esp+4]` is `a`. The first hit has `b = 0`, and the next `c` stops again with `b = 1`.
So the continue really did execute past the breakpoint *and* re-arm it in between. If
the dance failed to re-arm, the next stop would never come. If it failed to step over,
`b` would still be 0.

## Why Not Hardware Breakpoints?

x86 has four debug registers (DR0–DR3) that trap on execute, write or read of an
address, with no memory patching at all. They work on read-only code and in ROM, and
they're how watchpoints work. They're also limited to four. Software breakpoints are
unlimited and portable, which is why every stub has them first. Adding `Z1`–`Z4` support
via DR7 is a good next step (see break-it).
