# Module 01: Why an In-Kernel Stub

## The Concept

When you debug a kernel in QEMU with `-s`, the debugger isn't really talking to your
kernel. It's talking to QEMU, and QEMU happens to be the CPU. When gdb asks for the
registers, QEMU reads its own data structures. When gdb sets a breakpoint, QEMU's
translator just stops before that instruction. Your kernel never knows.

```
 With QEMU -s                           On a real machine
 ────────────                           ─────────────────
 gdb ── TCP ── QEMU's gdbstub           gdb ── serial cable ── ??? 
                  │ (outside the CPU)                           │
                  ▼                                             ▼
               emulated CPU                                 real CPU
```

On a real machine there is no "outside". Your options:

- **A JTAG or SWD probe.** It works, but it's rare on PCs, needs hardware, and not every
  board exposes the port.
- **Print debugging.** It works until the bug is in the code that prints.
- **A stub inside the kernel.** A small piece of code that runs when the CPU traps, and
  explains the machine's state over a serial port to a debugger on another computer.

The third option is what this course builds. Linux calls its version `kgdb`. The idea is
older than Linux: debuggers on minicomputers worked this way. A stub's job is narrow and
well defined:

1. When the CPU takes an exception (a breakpoint, a single step, a crash), save its state.
2. Tell the debugger why it stopped.
3. Answer questions: registers, memory.
4. Apply changes: write registers or memory, insert breakpoints.
5. When told to continue or step, restore the (possibly edited) state and return.

The wire format is the **GDB Remote Serial Protocol**, the same protocol QEMU's `-s`
speaks. That's why one debugger can talk to QEMU, to OpenOCD, or to your kernel without
knowing the difference.

## The Setup

You don't need a second PC and a null-modem cable to develop a stub. QEMU can wire the
guest's first serial port, COM1 at I/O port `0x3F8`, to a TCP socket:

```
qemu-system-i386 -kernel kernel.elf -display none \
    -serial tcp:127.0.0.1:4444,server=on,wait=on
```

- `server=on`: QEMU listens, and the debugger connects to it.
- `wait=on`: QEMU doesn't start the CPU until someone connects. Without it, anything the
  kernel prints before you connect is lost.

Crucially, there's **no** `-s` here. Every byte of debugging in this course goes through
the kernel's own serial port, exactly as it would over a real cable. When it works in
QEMU, the only thing that changes on hardware is the cable.

## The Kernel

The course kernel is deliberately boring. It needs a function to break on, a loop to
watch, and a stack to walk:

```c
volatile unsigned int counter;

int add(int a, int b) {
    return a + b;
}

int compute(int n) {
    int total = 0;
    for (int i = 0; i < n; i++)
        total = add(total, i);
    return total;
}
```

`boot.s` holds a multiboot header so QEMU's `-kernel` loader (or GRUB) can load it. It
also sets up a 16 KiB stack and calls `kmain`. It's built with `-O0 -g
-fno-omit-frame-pointer`, so line numbers are exact and every function keeps a frame
pointer, which matters in Module 09's backtraces.

Builds use `python -m ziglang cc -target x86-freestanding-none`. That's clang plus a
linker in one pip package, so nobody has to build a cross compiler for a tutorial.

## The First Byte Out

Module 01's kernel does the minimum that proves the wire works:

```c
void serial_puts(const char *s) {
    while (*s)
        outb(0x3F8, (u8)*s++);
}
```

`outb` writes one byte to an I/O port. `0x3F8` is COM1's data register. There's no
initialisation and no check that the UART is ready. In QEMU this works because the
emulated UART accepts a byte instantly. On real hardware it *usually* works because the
BIOS left COM1 configured, but bytes get lost if you write faster than the line can send
them. Module 02 does it properly.

`kmain` prints and halts:

```c
serial_puts("hello from ring 0\n");
for (;;)
    __asm__ volatile("hlt");
```

## The Check

`check.py` connects to the TCP port, which is also what releases the CPU because of
`wait=on`, and waits for the line:

```
ok  kernel printed over COM1 -> QEMU -> TCP: 'hello from ring 0'
```

All the debugging in the rest of the course travels over this one path.

## Why Not Just Use QEMU's Stub?

You should, while developing in QEMU. It's more capable than anything in this course: it
can stop the CPU at any instruction without changing memory, it works before your kernel
has an IDT, and it can't be broken by your kernel's bugs. A stub inside the kernel can be
broken by those bugs, and later modules show exactly how.

Build one anyway, for two reasons:

- **It's the only option on real hardware.** A bug that only shows up on one laptop's
  firmware won't reproduce in QEMU.
- **It's the best way to understand a debugger.** After this course, "the debugger
  stopped at a breakpoint" won't be magic. It's an `int3` you wrote into memory, a trap
  frame you saved, and an `iret` you chose when to execute.

## Run It

```bash
python build.py build 01-why-an-in-kernel-stub kernel.elf
qemu-system-i386 -kernel kernel.elf -display none -serial tcp:127.0.0.1:4444,server=on,wait=on
# another terminal:
python -c "import socket; print(socket.create_connection(('127.0.0.1',4444)).recv(64))"
```

Or run `python verify.py -v 01`.
