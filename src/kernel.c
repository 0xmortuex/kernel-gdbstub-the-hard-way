/* kernel.c - the program being debugged. Built -O0 with frame pointers so
 * lines and backtraces are exact. */
//@if 1..1

void serial_puts(const char *s);
//@endif
//@if 2..3

void uart_init(void);
void uart_puts(const char *s);
unsigned char uart_getc(void);
void uart_putc(unsigned char c);
//@endif
//@if 3..3
void gdb_serve_demo(void);
//@endif
//@if 4

void uart_init(void);
void gdb_init(void);
void gdb_breakpoint(void);
//@endif
//@if 9
void gdb_poll(void);
//@endif

volatile unsigned int counter;
//@if 6
volatile unsigned int break_now;   /* the debugger writes 1 here to stop in kmain */
//@endif

int add(int a, int b) {
    return a + b;
}

int compute(int n) {
    int total = 0;
    for (int i = 0; i < n; i++)
        total = add(total, i);
    return total;
}

void kmain(void) {
//@if 1..1
    serial_puts("hello from ring 0\n");
    for (;;)
        __asm__ volatile("hlt");
//@endif
//@if 2..2
    uart_init();
    uart_puts("echo ready\n");
    for (;;)
        uart_putc(uart_getc());
//@endif
//@if 3..3
    uart_init();
    gdb_serve_demo();
//@endif
//@if 4
    uart_init();
    gdb_init();
    gdb_breakpoint();   /* wait here for the debugger */
    for (;;) {
        counter += (unsigned int)compute(5);
//@if 6
        if (break_now) {
            break_now = 0;
            gdb_breakpoint();
        }
//@endif
//@if 9
        gdb_poll();
//@endif
    }
//@endif
}
