/* kernel.c - the program being debugged, now in 64-bit long mode. Built -O0
 * with frame pointers so lines and backtraces are exact. */

void uart_init(void);
void gdb_init(void);
void gdb_breakpoint(void);
void gdb_poll(void);

volatile unsigned int counter;
volatile unsigned int break_now;   /* the debugger writes 1 here to stop in kmain */

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
    uart_init();
    gdb_init();
    gdb_breakpoint();   /* wait here for the debugger */
    for (;;) {
        counter += (unsigned int)compute(5);
        if (break_now) {
            break_now = 0;
            gdb_breakpoint();
        }
        gdb_poll();
    }
}
