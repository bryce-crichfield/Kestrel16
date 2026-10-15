// shell.c -- a small interactive shell for ISA-16
//
// Build:  ./isa16cc shell.c && ./isa16asm shell.s
// Run:    ./isa16 -n 0 shell.bin       (-n 0 = no step limit, it waits for input)
//
// Talks to the UART at 0xFF00 (data) / 0xFF02 (status, bit0 = byte ready).
// No preprocessor in isa16cc, so constants are enums.

enum { RX_READY = 1, LINE_MAX = 80, MAX_ARGS = 8, SIEVE_MAX = 1000 };

unsigned *uart_data = (unsigned *)0xFF00;
unsigned *uart_status = (unsigned *)0xFF02;
char *hexdigits = "0123456789ABCDEF";

char line[LINE_MAX];
char *args[MAX_ARGS];
char sieve[SIEVE_MAX + 1];
int running = 1;
int commandCount = 0;

// ---------------------------------------------------------------- I/O

void putch(int c)
{
    *uart_data = c;
}

void print(char *s)
{
    while (*s) putch(*s++);
}

int getch(void)
{
    while (!(*uart_status & RX_READY))
        ;
    return *uart_data & 255;
}

int readline(char *buf, int max)
{
    int n = 0;
    for (;;) {
        int c = getch();
        if (c == '\n' || c == '\r') break;
        if (c == 8 || c == 127) {          // backspace
            if (n > 0) n--;
            continue;
        }
        if (n < max - 1) buf[n++] = c;
    }
    buf[n] = 0;
    return n;
}

void print_uint(unsigned v)
{
    char tmp[6];
    int n = 0;
    do {
        tmp[n++] = '0' + v % 10;
        v = v / 10;
    } while (v);
    while (n > 0) putch(tmp[--n]);
}

void print_int(int v)
{
    if (v < 0) {
        putch('-');
        print_uint(-v);
    } else {
        print_uint(v);
    }
}

void print_hex(unsigned v, int digits)
{
    for (int i = digits - 1; i >= 0; i--)
        putch(hexdigits[(v >> (i * 4)) & 15]);
}

// ---------------------------------------------------------------- strings

int streq(char *a, char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

int digit_value(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// Decimal or 0x hex, optional leading '-'. Returns 1 on success.
int parse_num(char *s, int *out)
{
    int negative = 0, base = 10, digits = 0;
    unsigned v = 0;
    if (*s == '-') { negative = 1; s++; }
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) { base = 16; s += 2; }
    while (*s) {
        int d = digit_value(*s);
        if (d < 0 || d >= base) return 0;
        v = v * base + d;
        s++;
        digits++;
    }
    if (!digits) return 0;
    *out = negative ? -(int)v : (int)v;
    return 1;
}

int tokenize(char *s)
{
    int count = 0;
    for (;;) {
        while (*s == ' ' || *s == '\t') *s++ = 0;
        if (!*s || count == MAX_ARGS) break;
        args[count++] = s;
        while (*s && *s != ' ' && *s != '\t') s++;
    }
    return count;
}

int need_num(char *s, int *out)
{
    if (parse_num(s, out)) return 1;
    print("not a number: ");
    print(s);
    putch('\n');
    return 0;
}

// ---------------------------------------------------------------- commands

void cmd_help(int argc, char **argv);

void cmd_echo(int argc, char **argv)
{
    for (int i = 1; i < argc; i++) {
        if (i > 1) putch(' ');
        print(argv[i]);
    }
    putch('\n');
}

void cmd_calc(int argc, char **argv)
{
    int a, b, r;
    char *op;
    if (argc != 4) { print("usage: calc A op B   (op: + - * / % & | ^ << >>)\n"); return; }
    if (!need_num(argv[1], &a) || !need_num(argv[3], &b)) return;
    op = argv[2];
    if (streq(op, "+")) r = a + b;
    else if (streq(op, "-")) r = a - b;
    else if (streq(op, "*")) r = a * b;
    else if (streq(op, "/") || streq(op, "%")) {
        if (b == 0) { print("division by zero\n"); return; }
        r = streq(op, "/") ? a / b : a % b;
    }
    else if (streq(op, "&")) r = a & b;
    else if (streq(op, "|")) r = a | b;
    else if (streq(op, "^")) r = a ^ b;
    else if (streq(op, "<<")) r = a << b;
    else if (streq(op, ">>")) r = a >> b;
    else { print("unknown operator: "); print(op); putch('\n'); return; }
    print_int(r);
    print("  (0x");
    print_hex(r, 4);
    print(")\n");
}

void cmd_hex(int argc, char **argv)
{
    int v;
    if (argc != 2) { print("usage: hex N\n"); return; }
    if (!need_num(argv[1], &v)) return;
    print("dec ");
    print_int(v);
    print("  unsigned ");
    print_uint(v);
    print("  hex 0x");
    print_hex(v, 4);
    print("  bin ");
    for (int i = 15; i >= 0; i--) {
        putch(((unsigned)v >> i) & 1 ? '1' : '0');
        if (i == 8) putch('_');
    }
    putch('\n');
}

void cmd_peek(int argc, char **argv)
{
    int addr, count = 16;
    if (argc < 2 || argc > 3) { print("usage: peek ADDR [COUNT]\n"); return; }
    if (!need_num(argv[1], &addr)) return;
    if (argc == 3 && !need_num(argv[2], &count)) return;
    unsigned char *p = (unsigned char *)addr;
    for (int i = 0; i < count; i++) {
        if (i % 8 == 0) {
            if (i) putch('\n');
            print_hex((unsigned)(p + i), 4);
            print(":");
        }
        putch(' ');
        print_hex(p[i], 2);
    }
    putch('\n');
}

void cmd_poke(int argc, char **argv)
{
    int addr, value;
    if (argc != 3) { print("usage: poke ADDR BYTE\n"); return; }
    if (!need_num(argv[1], &addr) || !need_num(argv[2], &value)) return;
    *(unsigned char *)addr = value;
}

void cmd_fib(int argc, char **argv)
{
    int n = 10;
    unsigned a = 0, b = 1, t;
    if (argc == 2 && !need_num(argv[1], &n)) return;
    if (n < 1 || n > 25) { print("pick 1..25 (16-bit limit)\n"); return; }
    for (int i = 0; i < n; i++) {
        print_uint(a);
        putch(i == n - 1 ? '\n' : ' ');
        t = a + b;
        a = b;
        b = t;
    }
}

void cmd_primes(int argc, char **argv)
{
    int n = 100, found = 0;
    if (argc == 2 && !need_num(argv[1], &n)) return;
    if (n < 2 || n > SIEVE_MAX) { print("pick 2..1000\n"); return; }
    for (int i = 0; i <= n; i++) sieve[i] = 1;
    for (int i = 2; i * i <= n; i++)
        if (sieve[i])
            for (int j = i * i; j <= n; j += i) sieve[j] = 0;
    for (int i = 2; i <= n; i++) {
        if (sieve[i]) {
            print_int(i);
            found++;
            putch(found % 10 == 0 ? '\n' : ' ');
        }
    }
    if (found % 10) putch('\n');
    print_int(found);
    print(" primes\n");
}

void cmd_rev(int argc, char **argv)
{
    for (int i = argc - 1; i >= 1; i--) {
        char *s = argv[i];
        int n = 0;
        while (s[n]) n++;
        while (n > 0) putch(s[--n]);
        if (i > 1) putch(' ');
    }
    putch('\n');
}

void cmd_clear(int argc, char **argv)
{
    print("\033[2J\033[H");
}

void cmd_about(int argc, char **argv)
{
    print("ISA-16 shell -- 16-bit CPU, 64 KB, UART at 0xFF00\n");
    print("commands run this session: ");
    print_int(commandCount);
    putch('\n');
}

void cmd_exit(int argc, char **argv)
{
    print("bye\n");
    running = 0;
}

struct Command {
    char *name;
    void (*run)(int, char **);
    char *help;
};

struct Command commands[] = {
    {"help",   cmd_help,   "list commands"},
    {"echo",   cmd_echo,   "echo TEXT...        print the arguments"},
    {"calc",   cmd_calc,   "calc A op B         integer math, e.g. calc 7 * 6"},
    {"hex",    cmd_hex,    "hex N               show N in dec/hex/binary"},
    {"peek",   cmd_peek,   "peek ADDR [COUNT]   hex dump memory"},
    {"poke",   cmd_poke,   "poke ADDR BYTE      write a byte to memory"},
    {"fib",    cmd_fib,    "fib [N]             first N Fibonacci numbers"},
    {"primes", cmd_primes, "primes [N]          primes up to N (sieve)"},
    {"rev",    cmd_rev,    "rev TEXT...         reverse text"},
    {"clear",  cmd_clear,  "clear the screen"},
    {"about",  cmd_about,  "about this machine"},
    {"exit",   cmd_exit,   "halt the machine"},
    {0, 0, 0}
};

void cmd_help(int argc, char **argv)
{
    for (struct Command *c = commands; c->name; c++) {
        print("  ");
        print(c->name);
        print(" - ");
        print(c->help);
        putch('\n');
    }
}

// ---------------------------------------------------------------- main

int main(void)
{
    print("\nISA-16 shell. Type 'help' for commands.\n");
    while (running) {
        print("isa16> ");
        readline(line, LINE_MAX);
        int argc = tokenize(line);
        if (argc == 0) continue;
        struct Command *c = commands;
        while (c->name && !streq(c->name, args[0])) c++;
        if (c->name) {
            commandCount++;
            c->run(argc, args);
        } else {
            print("unknown command: ");
            print(args[0]);
            print(" (try 'help')\n");
        }
    }
    return 0;
}