// Shell.h -- declarations for Shell.c, the small interactive shell for ISA-16

#ifndef SHELL_H
#define SHELL_H

// The UART: data at 0xFF00, status at 0xFF02 (bit 0 = a byte is ready).
#define UART_DATA   ((unsigned *)0xFF00)
#define UART_STATUS ((unsigned *)0xFF02)
#define RX_READY    1

#define LINE_MAX    80
#define MAX_ARGS    8
#define SIEVE_MAX   1000

struct Command {
    char *name;
    void (*run)(int, char **);
    char *help;
};

extern char line[LINE_MAX];
extern char *args[MAX_ARGS];
extern int running;
extern int commandCount;
extern struct Command commands[];

// I/O
void putch(int c);
void print(char *s);
int getch(void);
int readline(char *buf, int max);
void print_uint(unsigned v);
void print_int(int v);
void print_hex(unsigned v, int digits);

// strings
int streq(char *a, char *b);
int digit_value(int c);
int parse_num(char *s, int *out);
int tokenize(char *s);
int need_num(char *s, int *out);

// commands
void cmd_help(int argc, char **argv);
void cmd_echo(int argc, char **argv);
void cmd_calc(int argc, char **argv);
void cmd_hex(int argc, char **argv);
void cmd_peek(int argc, char **argv);
void cmd_poke(int argc, char **argv);
void cmd_fib(int argc, char **argv);
void cmd_primes(int argc, char **argv);
void cmd_rev(int argc, char **argv);
void cmd_clear(int argc, char **argv);
void cmd_about(int argc, char **argv);
void cmd_exit(int argc, char **argv);

#endif
