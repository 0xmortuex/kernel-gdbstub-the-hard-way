# Module 02: A Polled UART

## The Concept

The stub's only connection to the outside world is a serial port, so the serial driver
is the foundation everything else stands on. It has to work when nothing else does:
inside an exception handler, with interrupts off, when the kernel's own state may be
corrupt. That rules out anything clever. No interrupts, no buffers shared with the rest
of the kernel, no locks. Just **polling**: ask the hardware "can I?", and wait until it
says yes.

The PC's serial port is a **16550 UART** (or something pretending to be one). It's a
handful of 8-bit registers starting at I/O port `0x3F8`:

```
 offset   read                      write
 ──────   ────                      ─────
 +0       RBR receive buffer        THR transmit holding     (when LCR.DLAB=0)
 +0/+1    ──── divisor latch lo/hi (when LCR.DLAB=1) ────
 +1                                 IER interrupt enable
 +2       IIR interrupt id          FCR FIFO control
 +3                                 LCR line control (word length, DLAB)
 +4                                 MCR modem control (DTR, RTS)
 +5       LSR line status: bit 0 = data ready, bit 5 = THR empty
```

## Initialisation

```c
void uart_init(void) {
    outb(COM1 + UART_IER, 0x00);   /* no interrupts: we poll                 */
    outb(COM1 + UART_LCR, 0x80);   /* DLAB=1 to reach the divisor latch      */
    outb(COM1 + UART_DATA, 0x01);  /* divisor 1 = 115200 baud (lo byte)      */
    outb(COM1 + UART_IER, 0x00);   /*            (hi byte)                   */
    outb(COM1 + UART_LCR, 0x03);   /* DLAB=0, 8 data bits, no parity, 1 stop */
    outb(COM1 + UART_FCR, 0x00);   /* FIFOs OFF (see below)                  */
    outb(COM1 + UART_MCR, 0x03);   /* DTR + RTS                              */
}
```

Line by line:

- **IER = 0.** The UART can raise an interrupt when a byte arrives. We don't want that:
  the stub runs with interrupts disabled, and the rest of the kernel shouldn't see
  debugger traffic.
- **LCR = 0x80, then the divisor.** Registers 0 and 1 have two meanings. Setting the
  *Divisor Latch Access Bit* turns them into the baud-rate divisor. The UART's clock is
  1.8432 MHz / 16 = 115200, so divisor 1 is 115200 baud. QEMU ignores the baud rate, but
  a real cable doesn't.
- **LCR = 0x03.** Clear DLAB again. Set 8 data bits, no parity, 1 stop bit ("8N1"), the
  format gdb expects.
- **MCR = 0x03.** Assert DTR and RTS. Some cables and adapters won't pass data without
  them.

## Reading and Writing

```c
int uart_can_read(void) { return inb(COM1 + UART_LSR) & 0x01; }

u8 uart_getc(void) {
    while (!uart_can_read())
        ;
    return inb(COM1 + UART_DATA);
}

void uart_putc(u8 c) {
    while (!(inb(COM1 + UART_LSR) & 0x20))
        ;
    outb(COM1 + UART_DATA, c);
}
```

The Line Status Register is the whole driver. Bit 0 means a received byte is waiting in
RBR. Bit 5 means the transmit register is empty, so the next byte can be written without
overwriting one still being sent. That's the fix for Module 01's
fire-and-forget `outb`.

`uart_getc` spins forever, and that's intentional. When the stub is waiting for the
debugger, there's nothing else the machine should be doing. `uart_can_read` exists
separately for Module 09, where the running kernel peeks for a Ctrl-C without blocking.

## The FIFO Trap

A 16550 has 16-byte receive and transmit FIFOs. The textbook init enables them with
`FCR = 0xC7` ("enable, clear both, 14-byte trigger"). This course's first draft did
exactly that, and Module 03's check failed: the stub never acknowledged the first
packet.

Here's what happened. With `wait=on`, QEMU starts the CPU the moment the debugger
connects. The debugger immediately sends `$?#3f`. Those bytes arrive at the UART
*before* the kernel reaches `uart_init`, and enabling the FIFOs **resets them**,
discarding what already arrived. The stub lost the `$`, treated the rest as noise, and
waited forever for a packet that had already come and gone.

```
 debugger:  $?#3f ───────────►  UART RBR
 kernel:                 ...boot...  uart_init: FCR=0xC7  ← RBR cleared, '$' gone
 kernel:                                     get_packet: wait for '$' ... forever
```

The fix is to leave the FIFOs off. Without them, the UART holds one byte, and the sender
(QEMU, or the far end of a real cable with flow control) waits. Nothing is thrown away.

A real gdb would eventually notice the missing ack and retransmit, but relying on the
other side to paper over your bug is how stubs end up working with one debugger and
failing with another.

On real hardware at high baud rates, a FIFO does protect you from overruns while the
stub is busy. A stub that wants the FIFO should enable it as early as possible in boot,
before any debugger traffic can arrive. The break-it challenges explore that.

## The Check

The kernel prints a banner, then echoes everything it receives:

```c
uart_init();
uart_puts("echo ready\n");
for (;;)
    uart_putc(uart_getc());
```

```
ok  banner arrived after uart_init()
ok  4 bytes echoed back byte-for-byte
ok  200 bytes echoed back byte-for-byte
```

The 200-byte message is more than a FIFO would hold. It survives because polling with
flow control never drops a byte. The sender simply waits until the kernel reads.

## Why Not Interrupt-Driven?

An interrupt-driven driver lets the kernel do other work while bytes trickle in. That's
right for a terminal and wrong for a debugger. The stub must work when interrupts are
broken, when the IDT is half built, and from inside other exception handlers. Polling has
no dependencies, and that's the property you want in the one piece of code you use to
debug all the others.

The one place a stub does want an interrupt is break-in: Ctrl-C while the kernel runs.
Module 09 gets it by polling from the kernel's main loop instead, and explains the
trade-off.
