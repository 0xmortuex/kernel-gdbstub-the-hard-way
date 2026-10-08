/* kernel.c - the program being debugged. Built -O0 with frame pointers so
 * lines and backtraces are exact. */

void serial_puts(const char *s);

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
    serial_puts("hello from ring 0\n");
    for (;;)
        __asm__ volatile("hlt");
}
