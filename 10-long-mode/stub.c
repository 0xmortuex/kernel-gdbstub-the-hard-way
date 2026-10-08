/* stub.c - the in-kernel GDB stub, ported to x86-64 long mode (Module 10)
 * and taught about more than one CPU (Module 11).
 *
 * Everything from Modules 02-09 is here, widened to 64 bits: the polled
 * UART, packet framing, the trap frame, fault-safe memory, continue/step,
 * int3 breakpoints, target.xml and Ctrl-C break-in. Interrupts stay off; the
 * stub owns the machine while it runs.
 */

typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef unsigned long u64;

static inline void outb(u16 port, u8 v) { __asm__ volatile("outb %0, %1" : : "a"(v), "Nd"(port)); }
static inline u8 inb(u16 port) {
    u8 v;
    __asm__ volatile("inb %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}

static u64 rdmsr(u32 msr) {
    u32 lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((u64)hi << 32) | lo;
}

/* ---- Polled 16550 UART (unchanged from Module 02) -------------------------- */
#define COM1 0x3F8

void uart_init(void) {
    outb(COM1 + 1, 0x00);   /* no interrupts                         */
    outb(COM1 + 3, 0x80);   /* DLAB=1                                */
    outb(COM1 + 0, 0x01);   /* divisor 1 = 115200 baud               */
    outb(COM1 + 1, 0x00);
    outb(COM1 + 3, 0x03);   /* 8N1                                   */
    outb(COM1 + 2, 0x00);   /* FIFOs off (Module 02's trap)          */
    outb(COM1 + 4, 0x03);   /* DTR + RTS                             */
}

static int uart_can_read(void) { return inb(COM1 + 5) & 0x01; }

static u8 uart_getc(void) {
    while (!uart_can_read())
        ;
    return inb(COM1);
}

static void uart_putc(u8 c) {
    while (!(inb(COM1 + 5) & 0x20))
        ;
    outb(COM1, c);
}

/* ---- RSP framing (unchanged from Module 03) ---------------------------------- */
static const char hexchars[] = "0123456789abcdef";

static int hexval(u8 c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static char inbuf[4200];
static char outbuf[8400];

static int get_packet(void) {
    for (;;) {
        while (uart_getc() != '$')
            ;
        u8 sum = 0;
        int n = 0;
        int restart = 0;
        for (;;) {
            u8 c = uart_getc();
            if (c == '$') { restart = 1; break; }
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
            if (c == '-') break;
        }
    }
}

/* ---- Hex helpers, now for any register width ---------------------------------- */
static char *put_hex_byte(char *out, u8 b) {
    *out++ = hexchars[b >> 4];
    *out++ = hexchars[b & 0xF];
    return out;
}

/* `size` bytes of v, little-endian (x86 is little-endian, so is the wire). */
static char *put_hex_le(char *out, u64 v, int size) {
    for (int i = 0; i < size; i++)
        out = put_hex_byte(out, (u8)(v >> (8 * i)));
    return out;
}

static u64 parse_hex(const char **pp) {
    u64 v = 0;
    int d;
    while ((d = hexval((u8)**pp)) >= 0) {
        v = (v << 4) | (u64)d;
        (*pp)++;
    }
    return v;
}

static int parse_hex_le(const char *p, int size, u64 *out) {
    u64 v = 0;
    for (int i = 0; i < size; i++) {
        int hi = hexval((u8)p[2 * i]), lo = hexval((u8)p[2 * i + 1]);
        if (hi < 0 || lo < 0) return -1;
        v |= (u64)((hi << 4) | lo) << (8 * i);
    }
    *out = v;
    return 0;
}

/* ---- The trapped CPU state (Module 10) ---------------------------------------- */
/* What boot.s's isr_common leaves on the stack, lowest address first. In long
 * mode the CPU pushes SS:RSP even for a ring-0 trap, so RSP is in the frame -
 * and, unlike the 32-bit stub, it can be changed: iretq pops it. */
struct frame {
    u64 r15, r14, r13, r12, r11, r10, r9, r8;
    u64 rbp, rdi, rsi, rdx, rcx, rbx, rax;            /* pushed by isr_common */
    u64 vector, err;                                   /* stub + CPU           */
    u64 rip, cs, rflags, rsp, ss;                      /* pushed by the CPU    */
};

/* gdb's amd64 register numbering - target.xml below lists them in this order:
 *  0 rax  1 rbx  2 rcx  3 rdx  4 rsi  5 rdi  6 rbp  7 rsp  8-15 r8-r15
 * 16 rip 17 eflags 18 cs 19 ss 20 ds 21 es 22 fs 23 gs 24 fs_base 25 gs_base */
#define NUM_REGS 26

static int reg_size(int n) {
    if (n <= 16) return 8;               /* GPRs and rip                 */
    if (n <= 23) return 4;               /* eflags and the six selectors */
    return 8;                            /* fs_base, gs_base             */
}

static u16 read_seg(int which) {
    u16 v = 0;
    switch (which) {
    case 0: __asm__ volatile("mov %%ds, %0" : "=r"(v)); break;
    case 1: __asm__ volatile("mov %%es, %0" : "=r"(v)); break;
    case 2: __asm__ volatile("mov %%fs, %0" : "=r"(v)); break;
    default: __asm__ volatile("mov %%gs, %0" : "=r"(v)); break;
    }
    return v;
}

static u64 reg_get(struct frame *f, int n) {
    switch (n) {
    case 0: return f->rax;   case 1: return f->rbx;   case 2: return f->rcx;
    case 3: return f->rdx;   case 4: return f->rsi;   case 5: return f->rdi;
    case 6: return f->rbp;   case 7: return f->rsp;
    case 8: return f->r8;    case 9: return f->r9;    case 10: return f->r10;
    case 11: return f->r11;  case 12: return f->r12;  case 13: return f->r13;
    case 14: return f->r14;  case 15: return f->r15;
    case 16: return f->rip;  case 17: return f->rflags;
    case 18: return f->cs;   case 19: return f->ss;
    case 20: case 21: case 22: case 23: return read_seg(n - 20);
    case 24: return rdmsr(0xC0000100);   /* IA32_FS_BASE */
    default: return rdmsr(0xC0000101);   /* IA32_GS_BASE */
    }
}

/* RSP is writable now (iretq pops it); selectors and bases are not. */
static int reg_set(struct frame *f, int n, u64 v) {
    switch (n) {
    case 0: f->rax = v; return 0;   case 1: f->rbx = v; return 0;
    case 2: f->rcx = v; return 0;   case 3: f->rdx = v; return 0;
    case 4: f->rsi = v; return 0;   case 5: f->rdi = v; return 0;
    case 6: f->rbp = v; return 0;   case 7: f->rsp = v; return 0;
    case 8: f->r8 = v; return 0;    case 9: f->r9 = v; return 0;
    case 10: f->r10 = v; return 0;  case 11: f->r11 = v; return 0;
    case 12: f->r12 = v; return 0;  case 13: f->r13 = v; return 0;
    case 14: f->r14 = v; return 0;  case 15: f->r15 = v; return 0;
    case 16: f->rip = v; return 0;
    case 17: f->rflags = (f->rflags & ~0xFFFFFFFFul) | (v & 0xFFFFFFFF); return 0;
    default: return -1;
    }
}

/* ---- IDT: 16-byte gates in long mode (Module 10) --------------------------------- */
struct idt_gate {
    u16 off_lo, selector;
    u8 ist, type_attr;
    u16 off_mid;
    u32 off_hi, zero;
} __attribute__((packed));

struct idtr {
    u16 limit;
    u64 base;
} __attribute__((packed));

extern u64 isr_table[32];
static struct idt_gate idt[32];

static void load_idt(void) {
    struct idtr r;
    r.limit = sizeof(idt) - 1;
    r.base = (u64)idt;
    __asm__ volatile("lidt %0" : : "m"(r));
}

extern int safe_read_byte(u64 addr);
extern int safe_write_byte(u64 addr, u32 v);
extern char safe_read_insn[], safe_write_insn[], safe_fixup[];

void gdb_init(void) {
    for (int i = 0; i < 32; i++) {
        u64 h = isr_table[i];
        idt[i].off_lo = (u16)h;
        idt[i].selector = 0x08;          /* our 64-bit code segment (boot.s) */
        idt[i].ist = 0;
        idt[i].type_attr = 0x8E;         /* present, DPL0, 64-bit interrupt gate */
        idt[i].off_mid = (u16)(h >> 16);
        idt[i].off_hi = (u32)(h >> 32);
        idt[i].zero = 0;
    }
    load_idt();
}

void gdb_breakpoint(void) { __asm__ volatile("int3"); }

static int last_signal = 5;
static int debugger_waiting;
#define RFLAGS_TF 0x100

/* ---- Software breakpoints (as in Module 07) ---------------------------------------- */
#define MAX_BP 16
static struct { u64 addr; u8 orig; u8 active; } bps[MAX_BP];
static u64 stepping_over;
static int step_requested;

static int bp_find(u64 addr) {
    for (int i = 0; i < MAX_BP; i++)
        if (bps[i].active && bps[i].addr == addr) return i;
    return -1;
}

static int bp_insert(u64 addr) {
    if (bp_find(addr) >= 0) return 0;
    for (int i = 0; i < MAX_BP; i++) {
        if (!bps[i].active) {
            int orig = safe_read_byte(addr);
            if (orig < 0) return -1;
            if (safe_write_byte(addr, 0xCC) < 0) return -1;
            bps[i].addr = addr;
            bps[i].orig = (u8)orig;
            bps[i].active = 1;
            return 0;
        }
    }
    return -1;
}

static int bp_remove(u64 addr) {
    int i = bp_find(addr);
    if (i < 0) return -1;
    safe_write_byte(addr, bps[i].orig);
    bps[i].active = 0;
    return 0;
}

static void resume(struct frame *f, int step) {
    int i = bp_find(f->rip);
    step_requested = step;
    if (i >= 0) {
        safe_write_byte(f->rip, bps[i].orig);
        stepping_over = f->rip;
        f->rflags |= RFLAGS_TF;
    } else if (step) {
        f->rflags |= RFLAGS_TF;
    }
    debugger_waiting = 1;
}

/* ---- Target description: amd64 (Module 10) ----------------------------------------- */
#define REG64(n, t) "<reg name=\"" n "\" bitsize=\"64\" type=\"" t "\"/>"
#define REG32(n) "<reg name=\"" n "\" bitsize=\"32\" type=\"int32\"/>"
static const char target_xml[] =
    "<?xml version=\"1.0\"?>"
    "<!DOCTYPE target SYSTEM \"gdb-target.dtd\">"
    "<target><architecture>i386:x86-64</architecture>"
    "<feature name=\"org.gnu.gdb.i386.core\">"
    "<flags id=\"i386_eflags\" size=\"4\">"
    "<field name=\"CF\" start=\"0\" end=\"0\"/><field name=\"PF\" start=\"2\" end=\"2\"/>"
    "<field name=\"AF\" start=\"4\" end=\"4\"/><field name=\"ZF\" start=\"6\" end=\"6\"/>"
    "<field name=\"SF\" start=\"7\" end=\"7\"/><field name=\"TF\" start=\"8\" end=\"8\"/>"
    "<field name=\"IF\" start=\"9\" end=\"9\"/><field name=\"DF\" start=\"10\" end=\"10\"/>"
    "<field name=\"OF\" start=\"11\" end=\"11\"/>"
    "</flags>"
    "<reg name=\"rax\" bitsize=\"64\" type=\"int64\" regnum=\"0\"/>"
    REG64("rbx", "int64") REG64("rcx", "int64") REG64("rdx", "int64")
    REG64("rsi", "int64") REG64("rdi", "int64")
    REG64("rbp", "data_ptr") REG64("rsp", "data_ptr")
    REG64("r8", "int64") REG64("r9", "int64") REG64("r10", "int64") REG64("r11", "int64")
    REG64("r12", "int64") REG64("r13", "int64") REG64("r14", "int64") REG64("r15", "int64")
    REG64("rip", "code_ptr")
    "<reg name=\"eflags\" bitsize=\"32\" type=\"i386_eflags\"/>"
    REG32("cs") REG32("ss") REG32("ds") REG32("es") REG32("fs") REG32("gs")
    "</feature>"
    "<feature name=\"org.gnu.gdb.i386.linux\">"
    REG64("fs_base", "int64") REG64("gs_base", "int64")
    "</feature></target>";

static int starts_with(const char *s, const char *prefix) {
    while (*prefix)
        if (*s++ != *prefix++) return 0;
    return 1;
}

static void handle_xfer(const char *p) {
    if (!starts_with(p, "target.xml:")) { put_packet("E00"); return; }
    p += 11;
    u64 off = parse_hex(&p);
    if (*p++ != ',') { put_packet("E01"); return; }
    u64 len = parse_hex(&p);
    u64 total = sizeof(target_xml) - 1;
    if (len > 4000) len = 4000;
    char *o = outbuf;
    *o++ = (off + len >= total) ? 'l' : 'm';
    for (u64 i = off; i < total && i < off + len; i++) {
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

/* ---- Break-in (as in Module 09) ----------------------------------------------------- */
static volatile int pending_interrupt;

void gdb_poll(void) {
    if (uart_can_read() && uart_getc() == 0x03) {
        pending_interrupt = 1;
        __asm__ volatile("int3");
    }
}

static int signal_for(u64 vector) {
    switch (vector) {
    case 0: return 8;
    case 1: case 3: return 5;
    case 6: return 4;
    default: return 11;
    }
}

static void serve(struct frame *f) {
    for (;;) {
        get_packet();
        char *o = outbuf;
        const char *p = inbuf + 1;
        struct frame *sel = f;
        switch (inbuf[0]) {
        case '?':
            *o++ = 'S';
            o = put_hex_byte(o, (u8)last_signal);
            *o = 0;
            put_packet(outbuf);
            break;
        case 'g':
            for (int i = 0; i < NUM_REGS; i++)
                o = put_hex_le(o, reg_get(sel, i), reg_size(i));
            *o = 0;
            put_packet(outbuf);
            break;
        case 'G': {
            const char *q = inbuf + 1;
            for (int i = 0; i < NUM_REGS; i++) {
                u64 v;
                if (parse_hex_le(q, reg_size(i), &v) < 0) break;
                if (v != reg_get(sel, i)) reg_set(sel, i, v);
                q += 2 * reg_size(i);
            }
            put_packet("OK");
            break;
        }
        case 'p': {
            u64 n = parse_hex(&p);
            if (n >= NUM_REGS) { put_packet("E00"); break; }
            o = put_hex_le(o, reg_get(sel, (int)n), reg_size((int)n));
            *o = 0;
            put_packet(outbuf);
            break;
        }
        case 'P': {
            u64 n = parse_hex(&p), v;
            if (n >= NUM_REGS || *p++ != '=' || parse_hex_le(p, reg_size((int)n), &v) < 0
                || reg_set(sel, (int)n, v) < 0)
                put_packet("E01");
            else
                put_packet("OK");
            break;
        }
        case 'm': {
            u64 addr = parse_hex(&p);
            if (*p++ != ',') { put_packet("E01"); break; }
            u64 len = parse_hex(&p);
            if (len > (sizeof(outbuf) - 1) / 2) len = (sizeof(outbuf) - 1) / 2;
            u64 i;
            for (i = 0; i < len; i++) {
                int b = safe_read_byte(addr + i);
                if (b < 0) break;
                int k = bp_find(addr + i);
                if (k >= 0) b = bps[k].orig;
                o = put_hex_byte(o, (u8)b);
            }
            *o = 0;
            put_packet(i == 0 ? "E14" : outbuf);
            break;
        }
        case 'M': {
            u64 addr = parse_hex(&p);
            if (*p++ != ',') { put_packet("E01"); break; }
            u64 len = parse_hex(&p);
            if (*p++ != ':') { put_packet("E01"); break; }
            int ok = 1;
            for (u64 i = 0; i < len && ok; i++) {
                int hi = hexval((u8)p[2 * i]), lo = hexval((u8)p[2 * i + 1]);
                if (hi < 0 || lo < 0) { ok = 0; break; }
                u8 b = (u8)((hi << 4) | lo);
                int k = bp_find(addr + i);
                if (k >= 0) { bps[k].orig = b; continue; }
                if (safe_write_byte(addr + i, b) < 0) ok = 0;
            }
            put_packet(ok ? "OK" : "E14");
            break;
        }
        case 'c':
            resume(f, 0);
            return;
        case 's':
            resume(f, 1);
            return;
        case 'D':
            put_packet("OK");
            resume(f, 0);
            debugger_waiting = 0;
            return;
        case 'Z':
        case 'z': {
            if (p[0] != '0' || p[1] != ',') { put_packet(""); break; }
            p += 2;
            u64 addr = parse_hex(&p);
            int r = inbuf[0] == 'Z' ? bp_insert(addr) : bp_remove(addr);
            put_packet(r == 0 ? "OK" : "E01");
            break;
        }
        case 'q':
            if (starts_with(inbuf, "qSupported")) {
                put_packet("PacketSize=1000;qXfer:features:read+");
                break;
            }
            if (starts_with(inbuf, "qXfer:features:read:")) {
                handle_xfer(inbuf + 20);
                break;
            }
            put_packet("");
            break;
        default:
            put_packet("");
            break;
        }
    }
}

void gdb_trap(struct frame *f) {
    /* The stub probing a bad address (Module 05) - on whichever CPU owns it. */
    if ((f->vector == 13 || f->vector == 14) &&
        (f->rip == (u64)safe_read_insn || f->rip == (u64)safe_write_insn)) {
        f->rip = (u64)safe_fixup;
        return;
    }
    if (f->vector == 1 && stepping_over) {
        safe_write_byte(stepping_over, 0xCC);
        stepping_over = 0;
        if (!step_requested) {
            f->rflags &= ~RFLAGS_TF;
            return;
        }
    }
    if (f->vector == 3 && bp_find(f->rip - 1) >= 0)
        f->rip -= 1;
    last_signal = signal_for(f->vector);
    if (f->vector == 3 && pending_interrupt) {
        pending_interrupt = 0;
        last_signal = 2;
    }
    f->rflags &= ~RFLAGS_TF;
    if (debugger_waiting) {
        debugger_waiting = 0;
        char reply[4];
        reply[0] = 'S';
        reply[1] = hexchars[(last_signal >> 4) & 0xF];
        reply[2] = hexchars[last_signal & 0xF];
        reply[3] = 0;
        put_packet(reply);
    }
    serve(f);
}
