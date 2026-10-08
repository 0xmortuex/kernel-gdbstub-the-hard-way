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

/* ---- RSP framing (Module 03) ---------------------------------------------- */
static const char hexchars[] = "0123456789abcdef";

static int hexval(u8 c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static char inbuf[4200];   /* one decoded incoming packet, NUL-terminated  */

/* Receive one packet into inbuf, acknowledging it. Anything before a '$'
 * (stray acks, noise) is ignored; a bad checksum is NACKed with '-' so the
 * debugger resends. Returns the payload length. */
static int get_packet(void) {
    for (;;) {
        while (uart_getc() != '$')
            ;
        u8 sum = 0;
        int n = 0;
        int restart = 0;
        for (;;) {
            u8 c = uart_getc();
            if (c == '$') { restart = 1; break; }   /* resync: a new packet began */
            if (c == '#') break;
            if (n < (int)sizeof(inbuf) - 1) inbuf[n++] = (char)c;
            sum += c;
        }
        if (restart) continue;
        int hi = hexval(uart_getc());
        int lo = hexval(uart_getc());
        if (hi >= 0 && lo >= 0 && ((hi << 4) | lo) == sum) {
            uart_putc('+');
            inbuf[n] = 0;
            return n;
        }
        uart_putc('-');
    }
}

/* Send one packet and wait for the debugger's '+'. On '-', resend. */
static void put_packet(const char *data) {
    for (;;) {
        u8 sum = 0;
        uart_putc('$');
        for (const char *p = data; *p; p++) {
            uart_putc((u8)*p);
            sum += (u8)*p;
        }
        uart_putc('#');
        uart_putc((u8)hexchars[sum >> 4]);
        uart_putc((u8)hexchars[sum & 0xF]);
        for (;;) {
            u8 c = uart_getc();
            if (c == '+') return;
            if (c == '-') break;    /* resend */
        }
    }
}

/* Module 03 only: a packet server with no machine behind it yet. It proves
 * the framing works; Module 04 replaces it with a real exception handler. */
void gdb_serve_demo(void) {
    for (;;) {
        get_packet();
        if (inbuf[0] == '?')
            put_packet("S05");
        else
            put_packet("");   /* empty reply = "I don't support that" */
    }
}
