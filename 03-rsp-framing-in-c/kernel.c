/* kernel.c - the program being debugged. Built -O0 with frame pointers so
 * lines and backtraces are exact. */

void uart_init(void);
void uart_puts(const char *s);
unsigned char uart_getc(void);
void uart_putc(unsigned char c);
void gdb_serve_demo(void);

volatile unsigned int counter;

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
    gdb_serve_demo();
}
