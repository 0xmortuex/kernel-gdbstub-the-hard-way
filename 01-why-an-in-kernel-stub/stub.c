/* stub.c - a GDB Remote Serial Protocol stub that lives inside the kernel
 * and talks to the debugger over the first serial port (COM1).
 *
 * Everything here is polled and single-CPU: no interrupts are enabled, so
 * the stub owns the machine whenever it runs.
 */

typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;

static inline void outb(u16 port, u8 v) { __asm__ volatile("outb %0, %1" : : "a"(v), "Nd"(port)); }

/* Module 01: the least code that gets a byte out of the machine. Writing to
 * the UART's data register without any setup works in QEMU (and usually on
 * real hardware the BIOS left in a sane state) - Module 02 does it properly. */
void serial_puts(const char *s) {
    while (*s)
        outb(0x3F8, (u8)*s++);
}
