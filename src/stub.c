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
//@if 2
static inline u8 inb(u16 port) {
    u8 v;
    __asm__ volatile("inb %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}
//@endif
//@if 1..1

/* Module 01: the least code that gets a byte out of the machine. Writing to
 * the UART's data register without any setup works in QEMU (and usually on
 * real hardware the BIOS left in a sane state) - Module 02 does it properly. */
void serial_puts(const char *s) {
    while (*s)
        outb(0x3F8, (u8)*s++);
}
//@endif
//@if 2

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
//@endif
//@if 3

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
//@endif
//@if 3..3

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
//@endif
//@if 4

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
//@if 5

/* ---- Paging, so bad addresses really fault (Module 05) ----------------------- */
/* One 4 MiB page identity-maps 0-4 MiB; everything above is unmapped. */
static u32 page_dir[1024] __attribute__((aligned(4096)));

static void paging_init(void) {
    u32 v;
    page_dir[0] = 0x00000083;                   /* present | writable | 4 MiB */
    __asm__ volatile("mov %%cr4, %0" : "=r"(v));
    v |= 0x10;                                  /* CR4.PSE: allow 4 MiB pages */
    __asm__ volatile("mov %0, %%cr4" : : "r"(v));
    __asm__ volatile("mov %0, %%cr3" : : "r"(page_dir));
    __asm__ volatile("mov %%cr0, %0" : "=r"(v));
    v |= 0x80000000;                            /* CR0.PG */
    __asm__ volatile("mov %0, %%cr0" : : "r"(v));
}

extern int safe_read_byte(u32 addr);
extern int safe_write_byte(u32 addr, u32 v);
extern char safe_read_insn[], safe_write_insn[], safe_fixup[];
//@endif

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
//@if 5
    paging_init();
//@endif
}

/* A compiled-in breakpoint: the kernel stops here and waits for the debugger. */
void gdb_breakpoint(void) { __asm__ volatile("int3"); }

static int last_signal = 5;   /* answer to '?' */
//@if 6
static int debugger_waiting;  /* set by c/s: the debugger expects a stop reply */
#define EFLAGS_TF 0x100
//@endif
//@if 7

/* ---- Software breakpoints (Module 07) ----------------------------------------- */
#define MAX_BP 16
static struct { u32 addr; u8 orig; u8 active; } bps[MAX_BP];
static u32 stepping_over;      /* breakpoint lifted for one instruction */
static int step_requested;     /* was that step the user's 's', or our own? */

static int bp_find(u32 addr) {
    for (int i = 0; i < MAX_BP; i++)
        if (bps[i].active && bps[i].addr == addr) return i;
    return -1;
}

static int bp_insert(u32 addr) {
    if (bp_find(addr) >= 0) return 0;
    for (int i = 0; i < MAX_BP; i++) {
        if (!bps[i].active) {
            int orig = safe_read_byte(addr);
            if (orig < 0) return -1;
            if (safe_write_byte(addr, 0xCC) < 0) return -1;   /* int3 */
            bps[i].addr = addr;
            bps[i].orig = (u8)orig;
            bps[i].active = 1;
            return 0;
        }
    }
    return -1;
}

static int bp_remove(u32 addr) {
    int i = bp_find(addr);
    if (i < 0) return -1;
    safe_write_byte(addr, bps[i].orig);
    bps[i].active = 0;
    return 0;
}

/* Resume at f->eip. If a breakpoint sits there, lift it, execute exactly one
 * instruction under the trap flag, and put it back in the #DB that follows. */
static void resume(struct frame *f, int step) {
    int i = bp_find(f->eip);
    step_requested = step;
    if (i >= 0) {
        safe_write_byte(f->eip, bps[i].orig);
        stepping_over = f->eip;
        f->eflags |= EFLAGS_TF;
    } else if (step) {
        f->eflags |= EFLAGS_TF;
    }
    debugger_waiting = 1;
}
//@endif
//@if 6..6

static void resume(struct frame *f, int step) {
    if (step) f->eflags |= EFLAGS_TF;
    debugger_waiting = 1;
}
//@endif
//@if 8

/* ---- Target description (Module 08) ------------------------------------------ */
static const char target_xml[] =
    "<?xml version=\"1.0\"?>"
    "<!DOCTYPE target SYSTEM \"gdb-target.dtd\">"
    "<target><architecture>i386</architecture>"
    "<feature name=\"org.gnu.gdb.i386.core\">"
    "<flags id=\"i386_eflags\" size=\"4\">"
    "<field name=\"CF\" start=\"0\" end=\"0\"/><field name=\"PF\" start=\"2\" end=\"2\"/>"
    "<field name=\"AF\" start=\"4\" end=\"4\"/><field name=\"ZF\" start=\"6\" end=\"6\"/>"
    "<field name=\"SF\" start=\"7\" end=\"7\"/><field name=\"TF\" start=\"8\" end=\"8\"/>"
    "<field name=\"IF\" start=\"9\" end=\"9\"/><field name=\"DF\" start=\"10\" end=\"10\"/>"
    "<field name=\"OF\" start=\"11\" end=\"11\"/>"
    "</flags>"
    "<reg name=\"eax\" bitsize=\"32\" type=\"int32\" regnum=\"0\"/>"
    "<reg name=\"ecx\" bitsize=\"32\" type=\"int32\"/>"
    "<reg name=\"edx\" bitsize=\"32\" type=\"int32\"/>"
    "<reg name=\"ebx\" bitsize=\"32\" type=\"int32\"/>"
    "<reg name=\"esp\" bitsize=\"32\" type=\"data_ptr\"/>"
    "<reg name=\"ebp\" bitsize=\"32\" type=\"data_ptr\"/>"
    "<reg name=\"esi\" bitsize=\"32\" type=\"int32\"/>"
    "<reg name=\"edi\" bitsize=\"32\" type=\"int32\"/>"
    "<reg name=\"eip\" bitsize=\"32\" type=\"code_ptr\"/>"
    "<reg name=\"eflags\" bitsize=\"32\" type=\"i386_eflags\"/>"
    "<reg name=\"cs\" bitsize=\"32\" type=\"int32\"/>"
    "<reg name=\"ss\" bitsize=\"32\" type=\"int32\"/>"
    "<reg name=\"ds\" bitsize=\"32\" type=\"int32\"/>"
    "<reg name=\"es\" bitsize=\"32\" type=\"int32\"/>"
    "<reg name=\"fs\" bitsize=\"32\" type=\"int32\"/>"
    "<reg name=\"gs\" bitsize=\"32\" type=\"int32\"/>"
    "</feature></target>";

static int starts_with(const char *s, const char *prefix) {
    while (*prefix)
        if (*s++ != *prefix++) return 0;
    return 1;
}

/* qXfer:features:read:<annex>:<offset>,<length> -> 'm' + chunk (more
 * follows) or 'l' + chunk (last). Binary-escape the four special bytes. */
static void handle_xfer(const char *p) {
    const char *annex = "target.xml:";
    if (!starts_with(p, annex)) { put_packet("E00"); return; }
    p += 11;
    u32 off = parse_hex(&p);
    if (*p++ != ',') { put_packet("E01"); return; }
    u32 len = parse_hex(&p);
    u32 total = sizeof(target_xml) - 1;
    if (len > 4000) len = 4000;
    char *o = outbuf;
    u32 i = off;
    *o++ = (off + len >= total) ? 'l' : 'm';
    for (; i < total && i < off + len; i++) {
        char c = target_xml[i];
        if (c == '#' || c == '$' || c == '}' || c == '*') {
            *o++ = '}';
            c = (char)(c ^ 0x20);
        }
        *o++ = c;
    }
    *o = 0;
    put_packet(outbuf);
}
//@endif
//@if 9

/* ---- Break-in (Module 09) ------------------------------------------------------ */
/* With no UART interrupt, the kernel calls this from its main loop. Ctrl-C in
 * the debugger arrives as a raw 0x03 byte, outside any packet. */
static volatile int pending_interrupt;

void gdb_poll(void) {
    if (uart_can_read() && uart_getc() == 0x03) {
        pending_interrupt = 1;
        __asm__ volatile("int3");
    }
}
//@endif

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
//@if 5
        case 'm': {
            u32 addr = parse_hex(&p);
            if (*p++ != ',') { put_packet("E01"); break; }
            u32 len = parse_hex(&p);
            if (len > (sizeof(outbuf) - 1) / 2) len = (sizeof(outbuf) - 1) / 2;
            u32 i;
            for (i = 0; i < len; i++) {
                int b = safe_read_byte(addr + i);
                if (b < 0) break;                      /* faulted: stop here */
//@if 7
                int k = bp_find(addr + i);             /* hide our own int3s */
                if (k >= 0) b = bps[k].orig;
//@endif
                o = put_hex_byte(o, (u8)b);
            }
            *o = 0;
            put_packet(i == 0 ? "E14" : outbuf);       /* E14 = EFAULT */
            break;
        }
        case 'M': {
            u32 addr = parse_hex(&p);
            if (*p++ != ',') { put_packet("E01"); break; }
            u32 len = parse_hex(&p);
            if (*p++ != ':') { put_packet("E01"); break; }
            int ok = 1;
            for (u32 i = 0; i < len && ok; i++) {
                int hi = hexval((u8)p[2 * i]), lo = hexval((u8)p[2 * i + 1]);
                if (hi < 0 || lo < 0) { ok = 0; break; }
                u8 b = (u8)((hi << 4) | lo);
//@if 7
                int k = bp_find(addr + i);             /* keep the int3, update what it hides */
                if (k >= 0) { bps[k].orig = b; continue; }
//@endif
                if (safe_write_byte(addr + i, b) < 0) ok = 0;
            }
            put_packet(ok ? "OK" : "E14");
            break;
        }
//@endif
//@if 6
        case 'c':
            resume(f, 0);
            return;
        case 's':
            resume(f, 1);
            return;
        case 'D':
            put_packet("OK");
            resume(f, 0);
            debugger_waiting = 0;   /* detached: nobody wants a stop reply */
            return;
//@endif
//@if 7
        case 'Z':
        case 'z': {
            if (p[0] != '0' || p[1] != ',') { put_packet(""); break; }  /* sw only */
            p += 2;
            u32 addr = parse_hex(&p);
            int r = inbuf[0] == 'Z' ? bp_insert(addr) : bp_remove(addr);
            put_packet(r == 0 ? "OK" : "E01");
            break;
        }
//@endif
        case 'q':
//@if 8
            if (starts_with(inbuf, "qSupported")) {
                put_packet("PacketSize=1000;qXfer:features:read+");
                break;
            }
            if (starts_with(inbuf, "qXfer:features:read:")) {
                handle_xfer(inbuf + 20);
                break;
            }
//@endif
//@if 4..7
            if (inbuf[1] == 'S' && inbuf[2] == 'u') {   /* qSupported */
                put_packet("PacketSize=1000");
                break;
            }
//@endif
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
//@if 5
    /* A fault inside safe_read_byte/safe_write_byte is the stub probing a bad
     * address, not a kernel crash: resume at the fixup, which returns -1. */
    if ((f->vector == 13 || f->vector == 14) &&
        (f->eip == (u32)safe_read_insn || f->eip == (u32)safe_write_insn)) {
        f->eip = (u32)safe_fixup;
        return;
    }
//@endif
//@if 7
    if (f->vector == 1 && stepping_over) {
        safe_write_byte(stepping_over, 0xCC);   /* put the breakpoint back */
        stepping_over = 0;
        if (!step_requested) {                  /* that was our step: keep going */
            f->eflags &= ~EFLAGS_TF;
            return;
        }
    }
    /* int3 leaves EIP after the 1-byte instruction. If it was one of ours,
     * rewind so the debugger sees the breakpoint address. */
    if (f->vector == 3 && bp_find(f->eip - 1) >= 0)
        f->eip -= 1;
//@endif
    last_signal = signal_for(f->vector);
//@if 9
    if (f->vector == 3 && pending_interrupt) {
        pending_interrupt = 0;
        last_signal = 2;                        /* SIGINT */
    }
//@endif
//@if 6
    f->eflags &= ~EFLAGS_TF;                    /* a single step is over */
    if (debugger_waiting) {                     /* answer the pending c/s */
        char reply[4];
        reply[0] = 'S';
        reply[1] = hexchars[(last_signal >> 4) & 0xF];
        reply[2] = hexchars[last_signal & 0xF];
        reply[3] = 0;
        debugger_waiting = 0;
        put_packet(reply);
    }
//@endif
    serve(f);
}
//@endif
