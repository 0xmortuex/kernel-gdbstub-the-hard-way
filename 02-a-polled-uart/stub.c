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
static inline u8 inb(u16 port) {
    u8 v;
    __asm__ volatile("inb %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}

/* ---- Polled 16550 UART driver (Module 02) --------------------------------- */
#define COM1 0x3F8
#define UART_DATA 0      /* RBR on read, THR on write (DLAB=0)            */
#define UART_IER  1      /* interrupt enable (DLAB=0) / divisor hi (DLAB=1) */
#define UART_FCR  2      /* FIFO control                                   */
#define UART_LCR  3      /* line control: word length, DLAB                */
#define UART_MCR  4      /* modem control: DTR, RTS                        */
#define UART_LSR  5      /* line status: bit0 data ready, bit5 THR empty   */

void uart_init(void) {
    outb(COM1 + UART_IER, 0x00);   /* no interrupts: we poll                 */
    outb(COM1 + UART_LCR, 0x80);   /* DLAB=1 to reach the divisor latch      */
    outb(COM1 + UART_DATA, 0x01);  /* divisor 1 = 115200 baud (lo byte)      */
    outb(COM1 + UART_IER, 0x00);   /*            (hi byte)                   */
    outb(COM1 + UART_LCR, 0x03);   /* DLAB=0, 8 data bits, no parity, 1 stop */
    outb(COM1 + UART_FCR, 0x00);   /* FIFOs OFF: turning them on resets them,
                                      discarding bytes the debugger already sent */
    outb(COM1 + UART_MCR, 0x03);   /* DTR + RTS                              */
}

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

void uart_puts(const char *s) {
    while (*s)
        uart_putc((u8)*s++);
}
