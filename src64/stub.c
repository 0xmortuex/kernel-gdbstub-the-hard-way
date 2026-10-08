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
//@if 11

#define MAX_CPUS 8
static struct frame *cpu_frame[MAX_CPUS];  /* each CPU's trap frame while stopped */
static u64 cpu_fs_base[MAX_CPUS], cpu_gs_base[MAX_CPUS];
static int sel_thread;                     /* Hg: whose registers g/G/p/P use */
//@endif

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
//@if 10..10
    case 24: return rdmsr(0xC0000100);   /* IA32_FS_BASE */
    default: return rdmsr(0xC0000101);   /* IA32_GS_BASE */
//@endif
//@if 11
    /* The MSRs are per CPU: use the values captured when that CPU stopped. */
    case 24: return cpu_fs_base[sel_thread];
    default: return cpu_gs_base[sel_thread];
//@endif
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
//@if 11

/* ---- Many CPUs (Module 11) ---------------------------------------------------------- */
/* Local APIC: memory-mapped at 0xFEE00000 (boot.s maps that page uncached). */
#define LAPIC 0xFEE00000ul
static inline u32 lapic_read(u32 reg) { return *(volatile u32 *)(LAPIC + reg); }
static inline void lapic_write(u32 reg, u32 v) { *(volatile u32 *)(LAPIC + reg) = v; }
static int this_cpu(void) { return (int)(lapic_read(0x20) >> 24); }   /* APIC ID */

/* ICR (interrupt command register): high half picks the destination, the
 * write to the low half sends. Shorthand 11 = "all CPUs except me". */
#define ICR_LO 0x300
#define ICR_HI 0x310
#define ALL_BUT_SELF (3u << 18)
static void send_ipi(u32 cmd) {
    lapic_write(ICR_HI, 0);
    lapic_write(ICR_LO, cmd);
    while (lapic_read(ICR_LO) & (1u << 12))   /* delivery status: still sending */
        ;
}

static volatile int online[MAX_CPUS];         /* by APIC ID: started and running */
static volatile int cpus_started = 1;         /* the BSP counts itself */
u8 ap_stacks[MAX_CPUS][16384] __attribute__((aligned(16)));

/* How many CPUs does the firmware say exist? The MADT lists one Processor
 * Local APIC entry (type 0) per CPU, with bit 0 of its flags = enabled.
 * (acpi-the-hard-way builds this properly; here it's the minimum.) */
static int count_cpus(void) {
    for (u64 a = 0xE0000; a < 0x100000; a += 16) {
        const char *s = (const char *)a;
        if (!starts_with(s, "RSD PTR ")) continue;
        u64 rsdt = *(volatile u32 *)(a + 16);
        u32 len = *(volatile u32 *)(rsdt + 4);
        for (u32 i = 36; i + 4 <= len; i += 4) {
            u64 t = *(volatile u32 *)(rsdt + i);
            if (!starts_with((const char *)t, "APIC")) continue;
            u32 mlen = *(volatile u32 *)(t + 4);
            int n = 0;
            for (u32 off = 44; off + 2 <= mlen; off += ((volatile u8 *)t)[off + 1]) {
                volatile u8 *e = (volatile u8 *)(t + off);
                if (e[1] == 0) break;                         /* malformed: stop */
                if (e[0] == 0 && (*(volatile u32 *)(e + 4) & 1)) n++;
            }
            return n > MAX_CPUS ? MAX_CPUS : n;
        }
    }
    return 1;
}

extern char ap_trampoline[], ap_trampoline_end[];
extern volatile u32 ap_booting;
void ap_main(int cpu);

static void delay(u32 n) {
    for (volatile u32 i = 0; i < n; i++)
        __asm__ volatile("pause");
}

/* Wake every AP: INIT, then two Startup IPIs pointing at vector 0x08, i.e.
 * the 16-bit trampoline we copy to physical 0x8000 (boot.s). */
void smp_init(void) {
    int want = count_cpus();
    online[this_cpu()] = 1;
    if (want <= 1) return;
    u8 *dst = (u8 *)0x8000;
    for (char *p = ap_trampoline; p < ap_trampoline_end; p++)
        *dst++ = (u8)*p;
    ap_booting = 1;
    lapic_write(0xF0, lapic_read(0xF0) | 0x100);    /* spurious vector reg: APIC on */
    send_ipi(ALL_BUT_SELF | 0x4500);                 /* INIT, assert, level */
    delay(100000);
    send_ipi(ALL_BUT_SELF | 0x4600 | 0x08);          /* Startup IPI, vector 0x08 */
    delay(10000);
    send_ipi(ALL_BUT_SELF | 0x4600 | 0x08);          /* the spec says: send it twice */
    while (cpus_started < want)                      /* every AP checks in */
        __asm__ volatile("pause");
}

/* First C code on each AP (from boot.s's ap_entry, on its own stack). */
void gdb_ap_start(int slot) {
    (void)slot;
    int me = this_cpu();
    load_idt();                                      /* same IDT, per-CPU IDTR */
    online[me] = 1;
    __atomic_fetch_add(&cpus_started, 1, __ATOMIC_SEQ_CST);
    ap_main(me);
}

/* ---- Stopping the world --------------------------------------------------------------
 * One CPU owns the stub at a time (stub_lock). When it stops, it sends an NMI
 * to everyone else - an NMI gets through even with interrupts disabled - and
 * each of them parks in gdb_trap, publishing its trap frame, until released. */
static volatile int stub_lock;
static volatile int stub_owner = -1;
static volatile int parked;
static volatile u32 release_gen;
static int others_held;           /* still parked across our own step-over */

static void capture_bases(int me) {
    cpu_fs_base[me] = rdmsr(0xC0000100);
    cpu_gs_base[me] = rdmsr(0xC0000101);
}

static int online_count(void) {
    int n = 0;
    for (int i = 0; i < MAX_CPUS; i++) n += online[i];
    return n;
}

/* A CPU that trapped and is queueing for the stub (below) gets parked by the
 * owner's NMI while it waits. The debugger should see where that CPU's
 * KERNEL code was - the pending trap - not the inside of the stub. */
static struct frame *pending_frame[MAX_CPUS];

static void park(int me, struct frame *f) {
    cpu_frame[me] = pending_frame[me] ? pending_frame[me] : f;
    capture_bases(me);
    u32 gen = release_gen;
    __atomic_fetch_add(&parked, 1, __ATOMIC_SEQ_CST);
    while (release_gen == gen)
        __asm__ volatile("pause");
}

static void stop_others(void) {
    int want = online_count() - 1;
    if (want <= 0) return;
    send_ipi(ALL_BUT_SELF | 0x4400);                 /* delivery mode 100 = NMI, assert */
    while (parked < want)
        __asm__ volatile("pause");
}

static void release_others(void) {
    parked = 0;
    __atomic_fetch_add(&release_gen, 1, __ATOMIC_SEQ_CST);
}

static char *put_thread_list(char *o) {
    int first = 1;
    for (int i = 0; i < MAX_CPUS; i++) {
        if (!online[i]) continue;
        if (!first) *o++ = ',';
        o = put_hex_byte(o, (u8)(i + 1));            /* thread id = APIC ID + 1 */
        first = 0;
    }
    return o;
}

/* "T05thread:03;" - which CPU stopped, so a client can switch to it. */
static void stop_reply(int sig, int cpu) {
    char *o = outbuf;
    *o++ = 'T';
    o = put_hex_byte(o, (u8)sig);
    const char *t = "thread:";
    while (*t) *o++ = *t++;
    o = put_hex_byte(o, (u8)(cpu + 1));
    *o++ = ';';
    *o = 0;
    put_packet(outbuf);
}

static int stopped_cpu;   /* the CPU whose event the debugger was told about */
//@endif

static void serve(struct frame *f) {
    for (;;) {
        get_packet();
        char *o = outbuf;
        const char *p = inbuf + 1;
//@if 11
        struct frame *sel = cpu_frame[sel_thread];   /* Hg picks whose registers */
//@endif
//@if 10..10
        struct frame *sel = f;
//@endif
        switch (inbuf[0]) {
        case '?':
//@if 10..10
            *o++ = 'S';
            o = put_hex_byte(o, (u8)last_signal);
            *o = 0;
            put_packet(outbuf);
//@endif
//@if 11
            stop_reply(last_signal, stopped_cpu);
//@endif
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
//@if 11
            if (sel != f) { put_packet("E01"); break; }   /* step the stopped CPU only */
//@endif
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
//@if 11
        case 'H': {                                  /* Hg<id>: select a thread */
            if (inbuf[1] == 'c') { put_packet("OK"); break; }   /* c/s apply to all */
            if (inbuf[1] != 'g') { put_packet(""); break; }
            p = inbuf + 2;
            if (p[0] == '-' || (p[0] == '0' && p[1] == 0)) {   /* -1 / 0: "any" */
                sel_thread = stopped_cpu;
                put_packet("OK");
                break;
            }
            u64 t = parse_hex(&p);
            if (t == 0 || t > MAX_CPUS || !online[t - 1]) { put_packet("E22"); break; }
            sel_thread = (int)t - 1;
            put_packet("OK");
            break;
        }
        case 'T': {                                  /* is thread <id> alive? */
            u64 t = parse_hex(&p);
            put_packet(t >= 1 && t <= MAX_CPUS && online[t - 1] ? "OK" : "E01");
            break;
        }
//@endif
        case 'q':
            if (starts_with(inbuf, "qSupported")) {
                put_packet("PacketSize=1000;qXfer:features:read+");
                break;
            }
            if (starts_with(inbuf, "qXfer:features:read:")) {
                handle_xfer(inbuf + 20);
                break;
            }
//@if 11
            if (starts_with(inbuf, "qfThreadInfo")) {   /* first (only) chunk */
                *o++ = 'm';
                o = put_thread_list(o);
                *o = 0;
                put_packet(outbuf);
                break;
            }
            if (starts_with(inbuf, "qsThreadInfo")) {   /* next chunk: none */
                put_packet("l");
                break;
            }
            if (inbuf[1] == 'C' && inbuf[2] == 0) {     /* qC: current thread */
                *o++ = 'Q';
                *o++ = 'C';
                o = put_hex_byte(o, (u8)(stopped_cpu + 1));
                *o = 0;
                put_packet(outbuf);
                break;
            }
//@endif
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
//@if 11
    int me = this_cpu();
    if (f->vector == 2 && stub_owner != me) {   /* NMI from the stopping CPU: park */
        park(me, f);
        return;
    }
    /* Decide "was this one of our breakpoints?" NOW: while we queue for the
     * lock, the owner may remove it. */
    int hit_bp = f->vector == 3 && bp_find(f->rip - 1) >= 0;
    if (hit_bp)
        f->rip -= 1;                            /* back onto the patched byte */
    if (!(others_held && stub_owner == me)) {   /* a fresh stop, not our own step */
        pending_frame[me] = f;
        while (__atomic_exchange_n(&stub_lock, 1, __ATOMIC_ACQUIRE))
            __asm__ volatile("pause");
        pending_frame[me] = 0;
        stub_owner = me;
        stop_others();
    }
    others_held = 0;
    if (hit_bp && bp_find(f->rip) < 0) {
        /* Two CPUs hit the same breakpoint; the debugger saw the first and
         * removed it. This hit is stale: the original byte is back at rip,
         * so just run it - reporting it (or resuming at rip+1, past the
         * instruction the int3 replaced) would be wrong. gdb does the same. */
        goto leave;
    }
    cpu_frame[me] = f;
    capture_bases(me);
//@endif
    if (f->vector == 1 && stepping_over) {
        safe_write_byte(stepping_over, 0xCC);
        stepping_over = 0;
        if (!step_requested) {
            f->rflags &= ~RFLAGS_TF;
//@if 11
            goto leave;                         /* our own step: resume everyone */
//@endif
//@if 10..10
            return;
//@endif
        }
    }
//@if 10..10
    if (f->vector == 3 && bp_find(f->rip - 1) >= 0)
        f->rip -= 1;
//@endif
    last_signal = signal_for(f->vector);
    if (f->vector == 3 && pending_interrupt) {
        pending_interrupt = 0;
        last_signal = 2;
    }
    f->rflags &= ~RFLAGS_TF;
//@if 11
    stopped_cpu = me;
    sel_thread = me;
//@endif
    if (debugger_waiting) {
        debugger_waiting = 0;
//@if 10..10
        char reply[4];
        reply[0] = 'S';
        reply[1] = hexchars[(last_signal >> 4) & 0xF];
        reply[2] = hexchars[last_signal & 0xF];
        reply[3] = 0;
        put_packet(reply);
//@endif
//@if 11
        stop_reply(last_signal, me);
//@endif
    }
    serve(f);
//@if 11
    /* Stepping (ours or the user's) runs ONE instruction on THIS CPU: keep the
     * others parked and the lock held until the #DB brings us back. */
    if (f->rflags & RFLAGS_TF) {
        others_held = 1;
        return;
    }
leave:
    release_others();
    stub_owner = -1;
    __atomic_store_n(&stub_lock, 0, __ATOMIC_RELEASE);
//@endif
}
