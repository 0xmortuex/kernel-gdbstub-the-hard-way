# Module 09: Debugging It With an Agent

## The Concept

Everything is in place: framing, a trap frame, safe memory, continue and step,
breakpoints, a target description. This module adds the last piece, **break-in** (the
debugger's Ctrl-C), and then hands the kernel to a client that has never heard of this
stub: [gdbstub-mcp](https://github.com/0xmortuex/gdbstub-mcp), the MCP server an AI agent
uses to debug kernels. If its tools work unchanged against your stub, the stub speaks
the protocol, not just a dialect that happens to work with your own tests.

## Break-In Without Interrupts

When the kernel is running, the debugger may want it to stop *now*, not at the next
breakpoint. The protocol's answer is one raw byte, `0x03` (Ctrl-C), sent outside any
packet. The stub must notice it and trap.

The textbook way is the UART's receive interrupt: a byte arrives, IRQ 4 fires, the
handler sees `0x03` and executes `int3`. That needs the PIC programmed, interrupts
enabled, and an IRQ handler that cooperates with the rest of the kernel. A stub's whole
point is to depend on as little as possible, so this one **polls instead**, from the
kernel's main loop:

```c
static volatile int pending_interrupt;

void gdb_poll(void) {
    if (uart_can_read() && uart_getc() == 0x03) {
        pending_interrupt = 1;
        __asm__ volatile("int3");
    }
}
```

```c
for (;;) {
    counter += (unsigned int)compute(5);
    ...
    gdb_poll();
}
```

`gdb_trap` turns that `int3` into the right signal:

```c
if (f->vector == 3 && pending_interrupt) {
    pending_interrupt = 0;
    last_signal = 2;                        /* SIGINT */
}
```

`S02` means "stopped by SIGINT": the debugger asked, and here you are. gdb reports this as
"Program received signal SIGINT".

The trade-off is honest: break-in only works while the kernel reaches `gdb_poll()`. A
kernel stuck in an infinite loop elsewhere, or spinning with interrupts off, can't be
stopped this way. The interrupt-driven version has the same weakness in the second case.
That's what hardware NMI buttons and watchdogs are for. Linux's kgdb supports both, and
`sysrq-g` too.

## The Session

`check.py` calls gdbstub-mcp's tool functions directly, the same functions an agent
calls over MCP:

```
> debug_connect(name="hw", port=..., elf="kernel.elf")
Connected 'hw' to 127.0.0.1:54985 (i386, 16 registers).
Symbols: kernel.elf - 90 symbols, 1055 line-table entries
Stopped: SIGTRAP (breakpoint/step)
pc = 0x1003c4 <gdb_breakpoint+0x4> at stub.c:257
next: pop ebp
```

`i386, 16 registers` comes from Module 08's XML. The stop is the boot-time `int3`. The
client asked `?`, and Module 06's "never speak first" rule held.

```
> debug_break("hw", "add")
#1 sw breakpoint at 0x1015d0 <add> at kernel.c:12

> debug_continue("hw")
Stopped: SIGTRAP (breakpoint/step) - breakpoint #1 (add)
pc = 0x1015d0 <add> at kernel.c:12
next: push ebp
```

Three modules show up in those two lines. `Z0` patched the `int3` (Module 07). The hit
rewound EIP to exactly `add`, which is why gdbstub-mcp could match it to breakpoint #1.
And `next: push ebp` rather than `next: int3` is the shadowing from Module 07 hiding the
patch.

```
> debug_registers("hw", "eip,esp,eflags")
     eip = 0x001015d0  <add> at kernel.c:12
     esp = 0x00106fd8  <stack_bottom+0x3fd8>
  eflags = 0x00000093  [CF AF SF]

> debug_backtrace("hw", mode="fp")
#0  0x1015d0 <add> at kernel.c:12
#1  0x101621 <compute+0x31> at kernel.c:19
#2  0x10165f <kmain+0x1f> at kernel.c:28
#3  0x100017 <_start+0xb> at boot.s:25
```

ESP was rebuilt from the frame address (Module 04), and the flags were decoded from your
XML (Module 08). The backtrace walks the stack with Module 05's `m`, four frames deep,
with the right line for each caller.

```
> debug_continue("hw", 0.5)        (after deleting the breakpoint)
Still running after 0.5s (no breakpoint hit). ...

> debug_interrupt("hw")
Stopped: SIGINT (interrupted)
pc = 0x1003f5 <gdb_poll+0x25> at stub.c:387
next: pop ebp

> debug_disconnect("hw")
Disconnected 'hw'; the guest is running.
```

The interrupt arrived as a raw `0x03` on the serial line, `gdb_poll` caught it, and the
stop is reported inside `gdb_poll`, exactly where the kernel was when it noticed.
`debug_disconnect` removes breakpoints with `z0` and sends `D`, which the stub answers
`OK` before letting the kernel run free.

## What gdbstub-mcp Needed

Running a client you didn't write against your stub is the real test, because it uses
the protocol the way *it* wants to:

- It sends `qSupported` with features you don't have (`multiprocess+`, `swbreak+`). The
  stub ignores them, and the client adapts to what was advertised back.
- It reads memory in 0x800-byte chunks, so the 8 KiB `outbuf` from Module 05 matters.
- It reads `target.xml` with its own chunk size, so the `m`/`l` logic matters.
- It reads the function's first bytes to analyse prologues, and the disassembler reads
  16 bytes at the PC, so the shadowing matters.
- It detaches with plain `D` because the stub didn't offer `multiprocess`. Against QEMU,
  which does offer it, it sends `D;1`. Advertising a feature changes what the client
  sends.

## On Real Hardware

Nothing in the stub knows about QEMU. To debug a physical PC:

1. Boot the kernel with GRUB (it's multiboot), on a machine with a COM1 UART.
2. Connect a null-modem cable to another computer at 115200 8N1.
3. Point the debugger at the serial device: `target remote /dev/ttyUSB0` in gdb. For
   gdbstub-mcp, put a serial-to-TCP bridge (`socat TCP-LISTEN:4444 /dev/ttyUSB0,b115200,raw`)
   in front, since it connects over TCP.

The same packets now travel over a real wire, where the checksums from Module 03 and the
lossless UART setup from Module 02 stop being theory.

## The Whole Course in One Diff

Run `diff 08-*/stub.c 09-*/stub.c`: 16 lines of stub, plus one call in `kmain`. Run
`diff 03-*/stub.c 09-*/stub.c` and you're looking at the entire debugger. The final stub
is about 550 lines of commented C, and `boot.s` is 136 lines, most of it the 32-entry
vector table.
