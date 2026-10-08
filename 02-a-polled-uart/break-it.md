# Break It: The UART

## Challenge 1: Turn the FIFOs Back On

Change `FCR` to `0xC7` in `src/stub.c`, run `python build.py stage`, then
`python verify.py 03`. Watch Module 03's check time out waiting for the first ack.

**Hint:** The debugger's first packet arrives before `uart_init` runs, and enabling the
FIFOs resets them. Try enabling them in `_start`, before anything else. Does it pass
now? Is it *guaranteed* to pass, or is it still a race, just a shorter one?

## Challenge 2: Forget DLAB

Delete the `LCR = 0x80` line, so the divisor writes land in the data register and IER.
What does QEMU do? What would a real UART at its power-on divisor do?

**Hint:** QEMU ignores the baud rate, so it may still "work", and that's exactly why
emulators hide this class of bug. On hardware, mismatched baud rates produce garbage
characters, which RSP checksums turn into an endless stream of `-` NACKs.

## Challenge 3: Skip the THR-Empty Check

Make `uart_putc` write without polling LSR bit 5 and send a 4 KiB `m` reply. Count the
bytes the debugger receives.

**Hint:** In QEMU, nothing breaks. On a real UART, every byte written while one is still
transmitting overwrites it. The checksum fails, the debugger NACKs, the stub resends,
and it fails again, forever. Polling isn't optional on real hardware.

## Challenge 4: A Different UART

Your board has an MMIO UART (a PL011, or an 8250 at a memory address) instead of I/O
port `0x3F8`. What has to change?

**Hint:** Only `inb`/`outb` and the register offsets. Everything above `uart_getc` and
`uart_putc` is UART-agnostic. On x86 PCs without a legacy UART, the ACPI SPCR table
tells you where the console UART is (acpi-mcp can decode it).

→ [Next: Module 03](../03-rsp-framing-in-c/README.md)
