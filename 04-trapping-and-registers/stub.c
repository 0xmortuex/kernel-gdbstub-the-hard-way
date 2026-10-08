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

static char outbuf[8400];  /* one outgoing payload: 0x800-byte reads = 4096 hex */

/* ---- Hex helpers ----------------------------------------------------------- */
static char *put_hex_byte(char *out, u8 b) {
    *out++ = hexchars[b >> 4];
    *out++ = hexchars[b & 0xF];
    return out;
}

/* Registers travel as target-endian (little-endian) bytes. */
static char *put_hex_u32_le(char *out, u32 v) {
    for (int i = 0; i < 4; i++)
        out = put_hex_byte(out, (u8)(v >> (8 * i)));
    return out;
}

static u32 parse_hex(const char **pp) {
    u32 v = 0;
    int d;
    while ((d = hexval((u8)**pp)) >= 0) {
        v = (v << 4) | (u32)d;
        (*pp)++;
    }
    return v;
}

static int parse_hex_u32_le(const char *p, u32 *out) {
    u32 v = 0;
    for (int i = 0; i < 4; i++) {
        int hi = hexval((u8)p[2 * i]), lo = hexval((u8)p[2 * i + 1]);
        if (hi < 0 || lo < 0) return -1;
        v |= (u32)((hi << 4) | lo) << (8 * i);
    }
    *out = v;
    return 0;
}

/* ---- The trapped CPU state (Module 04) ------------------------------------- */
/* Exactly what boot.s's isr_common left on the stack, lowest address first. */
struct frame {
    u32 gs, fs, es, ds;                                    /* pushed by us    */
    u32 edi, esi, ebp, esp_at_pusha, ebx, edx, ecx, eax;   /* pusha           */
    u32 vector, err;                                       /* stub + CPU      */
    u32 eip, cs, eflags;                                   /* pushed by CPU   */
};

/* A ring-0 trap pushes no SS:ESP, so the interrupted code's ESP is simply
 * the address just above the EFLAGS slot the CPU pushed. */
static u32 trapped_esp(struct frame *f) { return (u32)&f->eflags + 4; }

static u32 read_ss(void) {
    u32 v;
    __asm__ volatile("mov %%ss, %0" : "=r"(v));
    return v;
}

#define NUM_REGS 16   /* gdb's i386 order: eax ecx edx ebx esp ebp esi edi eip eflags cs ss ds es fs gs */

static u32 reg_get(struct frame *f, int n) {
    switch (n) {
    case 0: return f->eax;   case 1: return f->ecx;   case 2: return f->edx;
    case 3: return f->ebx;   case 4: return trapped_esp(f);
    case 5: return f->ebp;   case 6: return f->esi;   case 7: return f->edi;
    case 8: return f->eip;   case 9: return f->eflags;
    case 10: return f->cs;   case 11: return read_ss();
    case 12: return f->ds;   case 13: return f->es;
    case 14: return f->fs;   default: return f->gs;
    }
}

/* Returns 0 on success. ESP can't be changed: the frame itself lives at the
 * old stack pointer, and iret would pop from the wrong place. Segment
 * registers are left alone - a bad selector would fault on the way out. */
static int reg_set(struct frame *f, int n, u32 v) {
    switch (n) {
    case 0: f->eax = v; return 0;   case 1: f->ecx = v; return 0;
    case 2: f->edx = v; return 0;   case 3: f->ebx = v; return 0;
    case 5: f->ebp = v; return 0;   case 6: f->esi = v; return 0;
    case 7: f->edi = v; return 0;   case 8: f->eip = v; return 0;
    case 9: f->eflags = v; return 0;
    default: return -1;
    }
}

/* ---- IDT (Module 04) --------------------------------------------------------- */
struct idt_gate {
    u16 off_lo, selector;
    u8 zero, type_attr;
    u16 off_hi;
} __attribute__((packed));

struct idtr {
    u16 limit;
    u32 base;
} __attribute__((packed));

extern u32 isr_table[32];
static struct idt_gate idt[32];

void gdb_init(void) {
    u16 cs;
    __asm__ volatile("mov %%cs, %0" : "=r"(cs));   /* reuse the loader's code segment */
    for (int i = 0; i < 32; i++) {
        u32 h = isr_table[i];
        idt[i].off_lo = (u16)(h & 0xFFFF);
        idt[i].selector = cs;
        idt[i].zero = 0;
        idt[i].type_attr = 0x8E;                    /* present, DPL0, 32-bit interrupt gate */
        idt[i].off_hi = (u16)(h >> 16);
    }
    struct idtr r;
    r.limit = sizeof(idt) - 1;
    r.base = (u32)idt;
    __asm__ volatile("lidt %0" : : "m"(r));
}

/* A compiled-in breakpoint: the kernel stops here and waits for the debugger. */
void gdb_breakpoint(void) { __asm__ volatile("int3"); }

static int last_signal = 5;   /* answer to '?' */

static int signal_for(u32 vector) {
    switch (vector) {
    case 0: return 8;          /* #DE  -> SIGFPE  */
    case 1: case 3: return 5;  /* #DB, #BP -> SIGTRAP */
    case 6: return 4;          /* #UD  -> SIGILL  */
    default: return 11;        /* #GP, #PF, ... -> SIGSEGV */
    }
}

/* Serve debugger requests until it says continue/step/detach. */
static void serve(struct frame *f) {
    for (;;) {
        get_packet();
        char *o = outbuf;
        const char *p = inbuf + 1;
        switch (inbuf[0]) {
        case '?':                               /* why are we stopped? */
            *o++ = 'S';
            o = put_hex_byte(o, (u8)last_signal);
            *o = 0;
            put_packet(outbuf);
            break;
        case 'g':
            for (int i = 0; i < NUM_REGS; i++)
                o = put_hex_u32_le(o, reg_get(f, i));
            *o = 0;
            put_packet(outbuf);
            break;
        case 'G': {
            for (int i = 0; i < NUM_REGS; i++) {
                u32 v;
                if (parse_hex_u32_le(inbuf + 1 + 8 * i, &v) < 0) break;
                if (i != 4 && v != reg_get(f, i)) reg_set(f, i, v);
            }
            put_packet("OK");
            break;
        }
        case 'p': {
            u32 n = parse_hex(&p);
            if (n >= NUM_REGS) { put_packet("E00"); break; }
            o = put_hex_u32_le(o, reg_get(f, (int)n));
            *o = 0;
            put_packet(outbuf);
            break;
        }
        case 'P': {
            u32 n = parse_hex(&p), v;
            if (*p++ != '=' || parse_hex_u32_le(p, &v) < 0 || reg_set(f, (int)n, v) < 0)
                put_packet("E01");
            else
                put_packet("OK");
            break;
        }
        case 'q':
            if (inbuf[1] == 'S' && inbuf[2] == 'u') {   /* qSupported */
                put_packet("PacketSize=1000");
                break;
            }
            put_packet("");
            break;
        default:
            put_packet("");   /* unsupported: empty reply */
            break;
        }
    }
}

/* Called by isr_common for every CPU exception, with the saved state. */
void gdb_trap(struct frame *f) {
    last_signal = signal_for(f->vector);
    serve(f);
}
