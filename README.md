# Kernel gdbstub — The Hard Way

Put a debugger *inside* your kernel. Over eleven modules you build the target side of the
GDB Remote Serial Protocol in C and assembly: a UART driver, packet framing, an exception
handler that hands the trapped CPU to a debugger, fault-proof memory access,
single-stepping, software breakpoints, and a target description. Then you take it to
64-bit long mode and to several CPUs at once. You debug the kernel through its own serial port with [gdbstub-mcp](https://github.com/0xmortuex/gdbstub-mcp)
or plain gdb. QEMU's built-in gdbstub is never turned on. Every line explained.

## Why This Exists

QEMU's `-s` flag gives you a perfect debugger for free. It works because QEMU *is* the
CPU: it can stop it, read it and rewrite it from the outside. Real hardware has no such
outside. Unless you own a JTAG probe and a board that exposes it, the only thing that
can stop a real CPU and describe what it was doing is code running *on* that CPU.

That code is a gdbstub. Linux has one (`kgdb`), and so do FreeBSD, Zephyr and most
embedded RTOSes. It's the classic way to debug a kernel on a real machine over a serial
cable. It's also one of the best ways to learn what a debugger actually is: a few hundred
lines that sit between an exception and an `iret` and answer questions.

[MCP Server — The Hard Way](https://github.com/0xmortuex/mcp-server-the-hard-way) built
the *client* side of this protocol. This course builds the other end of the same wire.

## Who This Is For

- Hobby OS developers who want to debug on real hardware
- Anyone who has used gdb and wondered what's on the other side of `target remote`
- Readers of the client-side course who want to finish the picture

**Prerequisites:** C, and a little x86 assembly. Interrupts, the IDT and the stack frame
are all explained where they appear. You need [QEMU](https://www.qemu.org/download/),
Python 3.10+, and `pip install -r requirements.txt`, which installs `ziglang` (a
self-contained clang, so there's no cross toolchain to set up) and gdbstub-mcp.

## Modules

| # | Module | What You'll Build |
|---|--------|-------------------|
| 01 | [Why an In-Kernel Stub](01-why-an-in-kernel-stub/) | The setup: a kernel, COM1 wired to a TCP socket, and why this is how real hardware gets debugged |
| 02 | [A Polled UART](02-a-polled-uart/) | A 16550 driver with no interrupts, and the FIFO trap that eats the debugger's first packet |
| 03 | [RSP Framing in C](03-rsp-framing-in-c/) | `$payload#cs`, acks, NACK-and-resend, and resynchronising on noise |
| 04 | [Trapping and Registers](04-trapping-and-registers/) | An IDT, an exception entry stub, the trap frame, and `?` `g` `G` `p` `P` |
| 05 | [Memory That Can't Crash the Stub](05-memory-that-cannot-crash-the-stub/) | `m`/`M`, paging, and an exception table, so a bad address answers `E14` |
| 06 | [Continue and Step](06-continue-and-step/) | `c`, `s`, the trap flag, `#DB`, and when to send a stop reply |
| 07 | [Software Breakpoints](07-software-breakpoints/) | Patching `int3`, rewinding EIP, the step-over dance, and hiding your own patches |
| 08 | [Telling the Debugger What It's Talking To](08-telling-the-debugger-what-it-is-talking-to/) | `qSupported` and `target.xml`, so a generic client learns your register layout |
| 09 | [Debugging It With an Agent](09-debugging-it-with-an-agent/) | gdbstub-mcp end to end against your stub, plus Ctrl-C over a serial line |
| 10 | [Long Mode](10-long-mode/) | A 32-bit trampoline into long mode, the 64-bit trap frame, the amd64 `target.xml`, and the same client working unchanged |
| 11 | [Many CPUs](11-many-cpus/) | Waking APs with INIT-SIPI-SIPI, stopping the world with NMIs, CPUs as RSP threads, and the stale-breakpoint race |

Each module contains:
- **`README.md`**: the concepts and design decisions
- **`stub.c`, `kernel.c`, `boot.s`, `linker.ld`**: the complete kernel at that stage
- **`check.py`**: talks to that kernel over its serial port and asserts what the README claims
- **`break-it.md`**: challenges that break your own stub, with hints

## One Kernel, Eleven Stages

There is one copy of the 32-bit kernel, in [`src/`](src/), and one of the 64-bit kernel
(Modules 10–11), in [`src64/`](src64/), each with stage markers:

```c
//@if 5            kept from Module 05 on
//@if 3..3         kept only in Module 03
//@endif
```

`python build.py stage` writes each module a clean copy at its stage, with no markers. So
`05-memory-that-cannot-crash-the-stub/stub.c` is exactly the stub as it stands in Module
05. Diff two neighbouring modules to see precisely what a module added.

## Run It

```bash
pip install -r requirements.txt
python build.py build 09-debugging-it-with-an-agent kernel.elf

# COM1 becomes a TCP server; wait=on holds the CPU until a debugger connects
qemu-system-i386 -kernel kernel.elf -display none \
    -serial tcp:127.0.0.1:4444,server=on,wait=on

# then, from gdb:           target remote localhost:4444
# or from an agent:         debug_connect(name="hw", port=4444, elf="kernel.elf")
```

For the 64-bit modules, boot the flat image `build.py` prints with `qemu-system-x86_64`
(add `-smp 4` for Module 11). QEMU's multiboot loader refuses ELF64, so the kernel uses
the multiboot "a.out kludge". Debuggers still read symbols from the ELF.

On real hardware the same kernel talks over a real COM1. Point gdb at the serial
device (`target remote /dev/ttyUSB0`) at 115200 baud.

## Every Module Is Verified

```bash
python verify.py        # or: python verify.py -v 09
```

For each module, `verify.py`:
1. checks the module's sources still match `src/`
2. builds its kernel
3. boots it in QEMU with COM1 on a TCP socket, and **without** QEMU's gdbstub (Module 11
   boots twice, with `-smp 2` and `-smp 4`)
4. runs its `check.py`

Modules 09 and 10 drive gdbstub-mcp's own agent tools against the kernel, in 32-bit and
64-bit mode. CI runs all of
this on every push.

## Architecture Overview

```
 debugger (gdb, gdbstub-mcp, ...)
        │  RSP: $g#67  $m100000,40#..  $Z0,1015d0,1#..  0x03
        ▼
 TCP socket ── QEMU -serial tcp ── COM1 (16550)     (real hardware: a serial cable)
                                      │  polled: LSR bit 0 / bit 5       ← Module 02
 ┌────────────────────────────────────┴──────────────────────────────┐
 │ kernel                                                             │
 │   int3 / #DB / #PF / any exception                                 │
 │        │ IDT gate → isr_N → isr_common (pusha, segs)  ← Module 04  │
 │        ▼                                                           │
 │   gdb_trap(struct frame *f)                                        │
 │     ├─ exception-table fixup for faulting reads      ← Module 05   │
 │     ├─ breakpoint rewind / step-over re-arm          ← Module 07   │
 │     ├─ stop reply if the debugger is waiting         ← Module 06   │
 │     └─ serve(): get_packet → ? g G p P m M c s Z z q ← Modules 03-08│
 │        ▲                                                           │
 │   iret back into the kernel with the (edited) frame                │
 └────────────────────────────────────────────────────────────────────┘
```

## Key Design Principles

**1. The stub owns the machine while it runs.** Interrupts stay off and nothing else runs.
That's what makes a polled, single-threaded stub correct, and it's also why it can't
debug code that needs interrupts to make progress.

**2. Never trust an address.** The debugger can ask for any memory. A stub that faults on
`m40000000,4` takes the whole machine down with it. Every access goes through a path
that can fail and report `E14`.

**3. Never let the debugger see your machinery.** A breakpoint is an `int3` you wrote
into the code. Reads must show the original byte, a hit must report the breakpoint's
address rather than the address after it, and continuing must step over it invisibly.

**4. Describe yourself, don't make the client guess.** `target.xml` turns "a stub that
works with my client" into "a stub that works with any client".

## What's Simplified

The stub is polled, and one CPU owns it at a time. Real stubs add these:
- interrupt-driven break-in, via the UART IRQ, instead of polling from the main loop
  (only the boot CPU polls, so Ctrl-C always stops thread 1)
- hardware breakpoints and watchpoints (DR0–DR7)
- `vCont`, for per-thread continue and step, and binary `X` writes
- x2APIC, and more than 8 CPUs: the stub uses each CPU's APIC ID directly as its slot,
  which works on QEMU, where IDs are 0, 1, 2…, but not on machines with sparse IDs

Each module's `break-it.md` points at where those would go.

## License

MIT. Use it, modify it, ship it.

---

Built by [0xmortuex](https://github.com/0xmortuex)
