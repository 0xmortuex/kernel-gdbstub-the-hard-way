# Break It: Long Mode

## Challenge 1: Bring Back the Red Zone

Remove `-mno-red-zone` from `build.py`, rebuild Module 10, and run its check a few
times. Then put a breakpoint inside `add` instead of on its first instruction.

**Hint:** At `-O0`, `add` is a leaf function that may keep `a` and `b` below RSP without
moving it. An `int3` there makes the CPU push a 40-byte frame right on top of them. The
debugger reads correct registers, but when the kernel resumes, `add` returns garbage and
`counter` stops making sense. The bug only shows when a trap lands in a leaf function,
which is exactly why it's so hard to find without knowing the rule.

## Challenge 2: Trust the 32-bit Stack Pointer

Delete `mov $stack_top, %rsp` from `long_start`, so the 64-bit code runs on whatever
RSP was left by the 32-bit `mov $stack_top, %esp`.

**Hint:** On QEMU it will probably work, because the upper half of RSP happens to be
zero. The architecture doesn't promise that: the upper 32 bits of registers are
undefined after a switch from compatibility mode. Code that works by accident on one
emulator is the kind that fails on real hardware. Can you find documentation that
QEMU zero-extends?

## Challenge 3: A Gate With the Wrong Selector

Change `idt[i].selector = 0x08` to `0x10`, the data segment, and run the check.

**Hint:** The very first `int3` needs a gate, and loading a data selector into CS is a
#GP. Delivering that #GP needs the same broken gate, so it becomes a #DF, then a triple
fault, and QEMU exits. Run it with `-d int,cpu_reset -D int.log -no-reboot` and feed the
log to gdbstub-mcp's `debug_explain_fault`: it names the selector and IDT entry.

## Challenge 4: Lie About the Width

In `target.xml`, change `rip` to `bitsize="32"` but keep sending 8 bytes for it in `g`.

**Hint:** Every register after `rip` is now read from the wrong offset: `eflags` takes
`rip`'s upper half, `cs` takes `eflags`, and so on. gdbstub-mcp will report a confident
`pc` that's actually the top four bytes of rip (zero), and a backtrace that starts
nowhere. The XML and the `g` layout are a single contract, and the client can't detect
a mismatch.

## Challenge 5: Map Less

Shrink the identity map from 512 large pages to 2, covering 0–4 MiB, and run the check.

**Hint:** The kernel itself still fits, so does the `m40000000` test still pass? Yes,
and so does a read at `0x400000`, which is now unmapped too. Then try a `-m 512` VM and
read the top of RAM: the stub answers `E14` for memory that exists. "Unmapped" and
"doesn't exist" look the same through the stub, and only the page tables decide.

→ [Next: Module 11](../11-many-cpus/README.md)
