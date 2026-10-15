// Fibonacci.c -- print every Fibonacci number that fits in 16 bits.
// The C version of Fibonacci.s, built with our own compiler (no preprocessor, no libc).

volatile unsigned char *const UART = (volatile unsigned char *)0xFF00;

void putc(char c)
{
    *UART = c;
}

void puts(const char *s)
{
    while (*s)
        putc(*s++);
}

void print_decimal(unsigned n)
{
    char digits[6];
    int count = 0;
    do {
        digits[count++] = '0' + n % 10;
        n /= 10;
    } while (n);
    while (count)
        putc(digits[--count]);
}

int main(void)
{
    unsigned a = 0, b = 1;

    puts("Fibonacci numbers that fit in 16 bits:\n");
    for (;;) {
        print_decimal(a);
        putc(' ');
        if (a + b < b) // the sum wrapped: b is the last one that fits
            break;
        unsigned next = a + b;
        a = b;
        b = next;
    }
    print_decimal(b);
    putc('\n');
    return 0;
}
