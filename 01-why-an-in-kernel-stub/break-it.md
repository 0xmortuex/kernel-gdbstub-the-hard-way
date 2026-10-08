# Break It: The Wire

## Challenge 1: Drop `wait=on`

Boot with `-serial tcp:127.0.0.1:4444,server=on,wait=off` and connect a second later.
Where did "hello from ring 0" go?

**Hint:** With `wait=off`, the CPU starts immediately. QEMU's socket chardev discards
output while no client is connected, so the line is gone before you arrive. Real serial
lines behave the same way: a terminal that's not listening misses the bytes. That's why
a stub never sends anything until the debugger has asked for it.

## Challenge 2: Print Faster Than the Line

On a real 16550 at 9600 baud, a byte takes about 1 ms to transmit, but `outb` returns in
nanoseconds. Write 1000 bytes with this module's `serial_puts` and count how many arrive.

**Hint:** Without polling the Line Status Register's "transmit holding register empty"
bit, each new `outb` overwrites a byte the UART hasn't sent yet. QEMU hides this because
its emulated UART accepts bytes instantly. Module 02 adds the check.

## Challenge 3: Two Kernels, One Port

Start QEMU, connect, disconnect, then connect again. What does the second connection
see?

**Hint:** The kernel already printed and halted, so nothing more is coming. A stub has to
cope with debuggers that come and go. The protocol's answer is that the debugger always
speaks first. On (re)connect it sends `qSupported` and `?`, and the stub replies.

## Challenge 4: Where Is COM1 on Your Machine?

Many laptops have no COM1. Find out what your hardware offers: a USB-serial adapter on a
desktop, a PCIe serial card, or AMT Serial-over-LAN on business laptops.

**Hint:** A USB-serial adapter can't be the *target's* port, because the kernel would need
a USB stack before it could debug anything. Look for a legacy UART at `0x3F8`, an MMIO
UART (described by the ACPI SPCR or DBG2 tables), or a PCI serial card. The stub only
needs `uart_getc` and `uart_putc`, so porting it is a two-function job.

→ [Next: Module 02](../02-a-polled-uart/README.md)
